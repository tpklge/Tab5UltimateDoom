#include "doomEsp_sound.h"
#include "esp_log.h"
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp_tab5.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"
#include "esp_codec_dev_defaults.h"

// Avoid conflict with stdbool.h included by ESP-IDF headers
#undef true
#undef false

#include "doomgeneric.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "esp_codec_dev_vol.h"
#include "esp_codec_dev_defaults.h"

// Variables from Doom to configure sound
extern int snd_sfxdevice;
extern int snd_musicdevice;

// Missing globals expected by i_sound.c when FEATURE_SOUND is on
int use_libsamplerate = 0;
float libsamplerate_scale = 1.0f;

static const char *TAG = "DOOM_AUDIO";

#define NUM_CHANNELS 16
#define MIXBUFFER_SAMPLES 512

typedef struct {
    const uint8_t *data;
    uint32_t length;
    uint32_t pos;
    int vol;
    int sep;
    bool playing;
} audio_channel_t;

static audio_channel_t channels[NUM_CHANNELS];
static esp_codec_dev_handle_t speaker_handle = NULL;
static TaskHandle_t mixer_task_handle = NULL;

static void audio_mixer_task(void *arg) {
    int16_t out_buffer[MIXBUFFER_SAMPLES * 2]; // Stereo
    
    // DOOM sound lumps are usually 11025 Hz, Mono, 8-bit unsigned
    // Codec is opened synchronously before this task is started.
    
    while (1) {
        memset(out_buffer, 0, sizeof(out_buffer));
        
        // Software mixing
        for (int c = 0; c < NUM_CHANNELS; c++) {
            if (channels[c].playing && channels[c].data) {
                audio_channel_t *ch = &channels[c];
                
                for (int i = 0; i < MIXBUFFER_SAMPLES; i++) {
                    if (ch->pos >= ch->length) {
                        ch->playing = false;
                        break; // Channel finished
                    }
                    
                    // Skip the 8-byte header in DMX sounds: (format(2), rate(2), num_samples(4))
                    int8_t sample = ch->data[ch->pos + 8] - 128; 
                    
                    // Convert 8-bit to 16-bit PCM but scale down (e.g. x64) 
                    // to leave headroom for up to 16 overlapping channels summing without clipping too hard.
                    int32_t sample16 = sample * 64; 
                    
                    // Calculate volume and panning (sep: 0 left, 255 right. Original doom uses it like 0 left, 254 right)
                    // We must avoid overflow in intermediate calculation
                    int32_t mixed_l = out_buffer[i*2] + ((sample16 * ch->vol * (255 - ch->sep)) / (15 * 255));
                    int32_t mixed_r = out_buffer[i*2 + 1] + ((sample16 * ch->vol * ch->sep) / (15 * 255));
                    
                    // Clamp
                    if (mixed_l > 32767) mixed_l = 32767;
                    if (mixed_l < -32768) mixed_l = -32768;
                    if (mixed_r > 32767) mixed_r = 32767;
                    if (mixed_r < -32768) mixed_r = -32768;
                    
                    out_buffer[i*2] = mixed_l;
                    out_buffer[i*2 + 1] = mixed_r;
                    
                    ch->pos++;
                }
            }
        }
        
        int result = esp_codec_dev_write(speaker_handle, out_buffer, sizeof(out_buffer));
        if (result != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Audio write failed: %d", result);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ----------------------------------------------------
// DOOM DG_sound_module Interface
// ----------------------------------------------------

// Available modules according to i_sound.h:
// SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, etc
static snddevice_t sound_devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS };

static boolean Sound_Init(boolean use_sfx_prefix) {
    ESP_LOGI(TAG, "Initializing Audio SFX Mixer");
    return true;
}

static void Sound_Shutdown(void) {
}

static int Sound_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
    char namebuf[12];
    sprintf(namebuf, "DS%s", sfxinfo->name);
    return W_GetNumForName(namebuf);
}

static void Sound_Update(void) {}

static void Sound_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel >= 0 && channel < NUM_CHANNELS) {
        channels[channel].vol = vol;
        channels[channel].sep = sep;
    }
}

static int Sound_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    int lump_size = W_LumpLength(sfxinfo->lumpnum);
    if (lump_size < 8) return -1;
    if (!sfxinfo->driver_data) {
        sfxinfo->driver_data = W_CacheLumpNum(sfxinfo->lumpnum, PU_STATIC);
    }
    
    int c = channel;
    if (c < 0 || c >= NUM_CHANNELS) {
        // Find free channel
        c = 0;
        for (int i = 0; i < NUM_CHANNELS; i++) {
            if (!channels[i].playing) { c = i; break; }
        }
    }
    
    uint8_t *b = (uint8_t*)sfxinfo->driver_data;
    uint16_t format = b[0] | (b[1]<<8);
    uint32_t num_samples = (uint32_t)b[4] | ((uint32_t)b[5]<<8) |
                           ((uint32_t)b[6]<<16) | ((uint32_t)b[7]<<24);
    
    if (format != 3 || num_samples > (uint32_t)lump_size - 8) {
        // Not a standard DOOM sound format
        return -1;
    }
    
    channels[c].data = (const uint8_t*)sfxinfo->driver_data;
    channels[c].length = num_samples;
    channels[c].pos = 0;
    channels[c].vol = vol;
    channels[c].sep = sep;
    channels[c].playing = true;
    static unsigned logged_sounds;
    if (logged_sounds++ < 5) {
        ESP_LOGI(TAG, "SFX %s: %lu samples, volume=%d, channel=%d",
                 sfxinfo->name, (unsigned long)num_samples, vol, c);
    }
    
    return c;
}

static void Sound_StopSound(int channel) {
    if (channel >= 0 && channel < NUM_CHANNELS) channels[channel].playing = false;
}

static boolean Sound_SoundIsPlaying(int channel) {
    if (channel >= 0 && channel < NUM_CHANNELS) return channels[channel].playing;
    return false;
}

static void Sound_CacheSounds(sfxinfo_t *sounds, int num_sounds) {}

sound_module_t DG_sound_module = {
    sound_devices, 3,
    Sound_Init, Sound_Shutdown, Sound_GetSfxLumpNum,
    Sound_Update, Sound_UpdateSoundParams, Sound_StartSound,
    Sound_StopSound, Sound_SoundIsPlaying, Sound_CacheSounds
};

// ----------------------------------------------------
// DOOM DG_music_module Interface (Dummy)
// ----------------------------------------------------
static snddevice_t music_devices[] = { SNDDEVICE_GENMIDI, SNDDEVICE_SOUNDCANVAS };

static boolean Music_Init(void) { return true; }
static void Music_Shutdown(void) {}
static void Music_SetMusicVolume(int volume) {}
static void Music_PauseMusic(void) {}
static void Music_ResumeMusic(void) {}
static void *Music_RegisterSong(void *data, int len) { return (void*)1; }
static void Music_UnRegisterSong(void *handle) {}
static void Music_PlaySong(void *handle, boolean looping) {}
static void Music_StopSong(void) {}
static boolean Music_MusicIsPlaying(void) { return false; }
static void Music_Poll(void) {}

music_module_t DG_music_module = {
    music_devices, 2,
    Music_Init, Music_Shutdown, Music_SetMusicVolume,
    Music_PauseMusic, Music_ResumeMusic, Music_RegisterSong,
    Music_UnRegisterSong, Music_PlaySong, Music_StopSong,
    Music_MusicIsPlaying, Music_Poll
};

// ----------------------------------------------------
// Hardware Initialization
// ----------------------------------------------------
void doomEsp_SoundInit(void) {
    ESP_LOGI(TAG, "Configuring ES8388 Codec and I2S for DOOM...");
    
    // bsp_audio_init will initialize I2S interface in EV-Board
    bsp_audio_init(NULL);
    speaker_handle = bsp_audio_codec_speaker_init();
    if (!speaker_handle) {
        ESP_LOGE(TAG, "Failed to initialize speaker codec!");
        return;
    }
    
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 11025, .channel = 2, .bits_per_sample = 16,
    };
    int result = esp_codec_dev_open(speaker_handle, &fs);
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Codec open failed: %d", result);
        return;
    }
    ESP_ERROR_CHECK(bsp_audio_configure_output());
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(speaker_handle, 70));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_mute(speaker_handle, false));
    ESP_LOGI(TAG, "Codec ready: 11025 Hz stereo, volume=70, unmuted; test tone");
    // Short, moderate-level 440 Hz tone to distinguish hardware output from SFX.
    int16_t tone[256 * 2];
    unsigned phase = 0;
    for (int block = 0; block < 20; block++) {
        for (int i = 0; i < 256; i++) {
            phase = (phase + 440) % 11025;
            int16_t value = phase < 5512 ? 1200 : -1200;
            tone[i * 2] = tone[i * 2 + 1] = value;
        }
        ESP_ERROR_CHECK(esp_codec_dev_write(speaker_handle, tone, sizeof(tone)));
    }
    
    for (int i = 0; i < NUM_CHANNELS; i++) channels[i].playing = false; // Initialize to zero
    
    BaseType_t created = xTaskCreatePinnedToCore(audio_mixer_task, "audio_mixer",
        8192, NULL, 5, &mixer_task_handle, 1);
    assert(created == pdPASS);
}
