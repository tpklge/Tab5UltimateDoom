#include "doomEsp.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/usb_host.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Rotate DOOM 90 degrees into the physical portrait framebuffer so the
 * Tab5 is used in landscape. PPA scale factors have 1/16 precision. */
static int g_scaled_w;
static int g_scaled_h;
static int g_out_x;
static int g_out_y;
static float g_scale;

static const char *TAG = "DOOM_ESP";

char doomEsp_savedir[32] = "/sdcard/doom/";

#define DOOM_W 320
#define DOOM_H 200

// Buffers
static uint16_t *doom_rb565;
static uint16_t *global_frame_buffer;
static ppa_client_handle_t ppa_client;
static bsp_p4_handles_t g_bsp_handles;

// Draw hook for doomgeneric
void p4_doom_draw_frame(const uint32_t *buffer) {
  memcpy(doom_rb565, buffer, DOOM_W * DOOM_H * 2);
  // PPA driver synchronizes the input and output cache windows.
  int64_t started = esp_timer_get_time();

  ppa_srm_oper_config_t srm_config = {
      .in = {.buffer  = doom_rb565,
             .pic_w   = DOOM_W,
             .pic_h   = DOOM_H,
             .block_w = DOOM_W,
             .block_h = DOOM_H,
             .srm_cm  = PPA_SRM_COLOR_MODE_RGB565},
      .out = {.buffer          = global_frame_buffer,
              .buffer_size     = LCD_H_RES * LCD_V_RES * 2,
              .pic_w           = LCD_H_RES,
              .pic_h           = LCD_V_RES,
              .block_offset_x  = g_out_x,
              .block_offset_y  = g_out_y,
              .srm_cm          = PPA_SRM_COLOR_MODE_RGB565},
      .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
      .scale_x = g_scale,
      .scale_y = g_scale,
      .mode = PPA_TRANS_MODE_BLOCKING,
  };

  ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(ppa_client, &srm_config));
  // Both PPA and LCD access RAM by DMA; no CPU writeback of the full
  // 1.84 MB output framebuffer is required after this operation.
  static int64_t report_start;
  static uint32_t frames, max_render_us;
  static uint64_t total_render_us;
  int64_t now = esp_timer_get_time();
  uint32_t render_us = (uint32_t)(now - started);
  if (!report_start) report_start = started;
  frames++;
  total_render_us += render_us;
  if (render_us > max_render_us) max_render_us = render_us;
  if (now - report_start >= 5000000) {
    ESP_LOGI(TAG, "Render: %.1f fps, PPA avg=%llu us max=%lu us",
             frames * 1000000.0 / (now - report_start),
             (unsigned long long)(total_render_us / frames),
             (unsigned long)max_render_us);
    report_start = now;
    frames = max_render_us = 0;
    total_render_us = 0;
  }
}

// Queue for keyboard events
typedef struct {
  int pressed;       // 1 if DOWN, 0 if UP
  unsigned char key; // DOOM key code
} doom_key_event_t;

static QueueHandle_t doom_key_queue;

static const uint8_t hid_to_doom[256] = {
    [0x04] = 'a',           [0x05] = 'b',           [0x06] = 'c',
    [0x07] = 'd',           [0x08] = 'e',           [0x09] = 'f',
    [0x0A] = 'g',           [0x0B] = 'h',           [0x0C] = 'i',
    [0x0D] = 'j',           [0x0E] = 'k',           [0x0F] = 'l',
    [0x10] = 'm',           [0x11] = 'n',           [0x12] = 'o',
    [0x13] = 'p',           [0x14] = 'q',           [0x15] = 'r',
    [0x16] = 's',           [0x17] = 't',           [0x18] = 'u',
    [0x19] = 'v',           [0x1A] = 'w',           [0x1B] = 'x',
    [0x1C] = 'y',           [0x1D] = 'z',           [0x1E] = '1',
    [0x1F] = '2',           [0x20] = '3',           [0x21] = '4',
    [0x22] = '5',           [0x23] = '6',           [0x24] = '7',
    [0x25] = '8',           [0x26] = '9',           [0x27] = '0',
    [0x28] = KEY_ENTER,     [0x29] = KEY_ESCAPE,    [0x2A] = KEY_BACKSPACE,
    [0x2B] = KEY_TAB,       [0x2C] = ' ',           [0x2D] = '-',
    [0x2E] = '=',           [0x2F] = '[',           [0x30] = ']',
    [0x31] = '\\',          [0x33] = ';',           [0x34] = '\'',
    [0x35] = '`',           [0x36] = ',',           [0x37] = '.',
    [0x38] = '/',           [0x3A] = KEY_F1,        [0x3B] = KEY_F2,
    [0x3C] = KEY_F3,        [0x3D] = KEY_F4,        [0x3E] = KEY_F5,
    [0x3F] = KEY_F6,        [0x40] = KEY_F7,        [0x41] = KEY_F8,
    [0x42] = KEY_F9,        [0x43] = KEY_F10,       [0x44] = KEY_F11,
    [0x45] = KEY_F12,       [0x48] = KEY_PAUSE,     [0x49] = KEY_INS,
    [0x4A] = KEY_HOME,      [0x4B] = KEY_PGUP,      [0x4C] = KEY_DEL,
    [0x4D] = KEY_END,       [0x4E] = KEY_PGDN,      [0x4F] = KEY_RIGHTARROW,
    [0x50] = KEY_LEFTARROW, [0x51] = KEY_DOWNARROW, [0x52] = KEY_UPARROW,
};

static void queue_doom_key(unsigned char key, int pressed) {
  if (!key)
    return;
  doom_key_event_t ev = {.pressed = pressed, .key = key};
  xQueueSend(doom_key_queue, &ev, 0);
}

/* Tab5 Keyboard Ext.Port1 protocol from M5Stack M5Unit-KEYBOARD:
 * 0x6D on SDA=GPIO0/SCL=GPIO1; normal mode gives press/release matrix events. */
static void tab5_keyboard_task(void *arg)
{
  (void)arg;
  i2c_master_bus_handle_t bus = NULL;
  const i2c_master_bus_config_t bus_cfg = {
      .i2c_port = I2C_NUM_1, .sda_io_num = GPIO_NUM_0,
      .scl_io_num = GPIO_NUM_1, .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
  if (i2c_master_probe(bus, 0x6D, 100) != ESP_OK) {
    ESP_LOGW(TAG, "Tab5 Keyboard not found at Ext.Port1 I2C 0x6D");
    i2c_del_master_bus(bus);
    vTaskDelete(NULL);
    return;
  }
  i2c_master_dev_handle_t device = NULL;
  const i2c_device_config_t dev_cfg = {
      .device_address = 0x6D, .scl_speed_hz = 100000,
  };
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &device));
  const uint8_t mode[] = {0x10, 0x00}; // Normal mode, clears event queue
  ESP_ERROR_CHECK(i2c_master_transmit(device, mode, sizeof(mode), 100));
  ESP_LOGI(TAG, "Tab5 Keyboard ready: WASD/arrows move, Ctrl fire, E/Space use, Aa run");
  static const uint8_t matrix_hid[70] = {
      0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x4C, 0x35, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2F, 0x30, 0x31, 0x2B, 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13, 0x33, 0x34, 0x2A, 0x00, 0x00, 0x04, 0x16, 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x52, 0x2D, 0x28, 0x00, 0x00, 0x1D, 0x1B, 0x06, 0x19, 0x05, 0x11, 0x10, 0x37, 0x50, 0x51, 0x4F, 0x2C
  };
  bool held[70] = {0};
  uint8_t references[256] = {0};
  bool reported_input = false;
  while (true) {
    for (int n = 0; n < 32; n++) {
      const uint8_t reg = 0x20;
      uint8_t event;
      if (i2c_master_transmit_receive(device, &reg, 1, &event, 1, 20) != ESP_OK)
        break;
      if (event == 0xFF) break;
      unsigned row = (event >> 4) & 7, col = event & 15;
      if (row >= 5 || col >= 14) continue;
      unsigned index = row * 14 + col;
      bool pressed = (event & 0x80) != 0;
      if (held[index] == pressed) continue;
      held[index] = pressed;
      unsigned char key = hid_to_doom[matrix_hid[index]];
      if (index == 43) key = KEY_RSHIFT; // Aa
      if (index == 56) key = KEY_FIRE;   // Ctrl
      if (index == 57) key = KEY_RALT;
      if (key == 'w') key = KEY_UPARROW;
      if (key == 'a') key = KEY_LEFTARROW;
      if (key == 's') key = KEY_DOWNARROW;
      if (key == 'd') key = KEY_RIGHTARROW;
      if (key == 'e' || key == ' ') key = KEY_USE;
      if (!key) continue;
      if (pressed) {
        if (references[key]++ == 0) queue_doom_key(key, 1);
      } else if (references[key] && --references[key] == 0) {
        queue_doom_key(key, 0);
      }
      if (!reported_input) {
        ESP_LOGI(TAG, "Tab5 Keyboard: first game key event received");
        reported_input = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// USB HID Host Callback
static void hid_host_keyboard_report_callback(const uint8_t *const data,
                                              const int length) {
  hid_keyboard_input_report_boot_t *kb =
      (hid_keyboard_input_report_boot_t *)data;
  if (length < sizeof(hid_keyboard_input_report_boot_t))
    return;

  static uint8_t prev_keys[HID_KEYBOARD_KEY_MAX] = {0};

  // Check released
  for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
    if (prev_keys[i]) {
      bool found = false;
      for (int j = 0; j < HID_KEYBOARD_KEY_MAX; j++)
        if (kb->key[j] == prev_keys[i])
          found = true;
      if (!found) {
        queue_doom_key(hid_to_doom[prev_keys[i]], 0);
        if (prev_keys[i] == 0x2C || prev_keys[i] == 0x08)
          queue_doom_key(KEY_USE, 0);
      }
    }
  }
  // Check pressed
  for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
    if (kb->key[i]) {
      bool found = false;
      for (int j = 0; j < HID_KEYBOARD_KEY_MAX; j++)
        if (prev_keys[j] == kb->key[i])
          found = true;
      if (!found) {
        queue_doom_key(hid_to_doom[kb->key[i]], 1);
        if (kb->key[i] == 0x2C || kb->key[i] == 0x08)
          queue_doom_key(KEY_USE, 1);
      }
    }
  }

  // Check modifiers
  static uint8_t old_mods = 0;
  uint8_t mods = kb->modifier.val;
  if ((mods & 0x11) && !(old_mods & 0x11)) {
    queue_doom_key(KEY_RCTRL, 1);
    queue_doom_key(KEY_FIRE, 1);
  }
  if (!(mods & 0x11) && (old_mods & 0x11)) {
    queue_doom_key(KEY_RCTRL, 0);
    queue_doom_key(KEY_FIRE, 0);
  }

  if ((mods & 0x22) && !(old_mods & 0x22))
    queue_doom_key(KEY_RSHIFT, 1);
  if (!(mods & 0x22) && (old_mods & 0x22))
    queue_doom_key(KEY_RSHIFT, 0);

  if ((mods & 0x44) && !(old_mods & 0x44))
    queue_doom_key(KEY_RALT, 1);
  if (!(mods & 0x44) && (old_mods & 0x44))
    queue_doom_key(KEY_RALT, 0);

  memcpy(prev_keys, kb->key, HID_KEYBOARD_KEY_MAX);
  old_mods = mods;
}

static void
hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                            const hid_host_interface_event_t event, void *arg) {
  if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
    size_t data_length;
    uint8_t data[64];
    ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(
        hid_device_handle, data, 64, &data_length));
    hid_host_dev_params_t dev_params;
    hid_host_device_get_params(hid_device_handle, &dev_params);
    if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
      hid_host_keyboard_report_callback(data, data_length);
    }
  } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
    hid_host_device_close(hid_device_handle);
  }
}

static void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                                     const hid_host_driver_event_t event,
                                     void *arg) {
  if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
    hid_host_dev_params_t dev_params;
    hid_host_device_get_params(hid_device_handle, &dev_params);
    if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
      const hid_host_device_config_t dev_config = {
          .callback = hid_host_interface_callback, .callback_arg = NULL};
      ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
      if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
        hid_class_request_set_protocol(hid_device_handle,
                                       HID_REPORT_PROTOCOL_BOOT);
        hid_class_request_set_idle(hid_device_handle, 0, 0);
      }
      ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
      ESP_LOGI(TAG, "USB Keyboard Configured and Ready!");
    }
  }
}

static void usb_lib_task(void *arg) {
  const usb_host_config_t host_config = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  ESP_ERROR_CHECK(usb_host_install(&host_config));
  xTaskNotifyGive(arg);
  while (true) {
    uint32_t event_flags;
    usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
  }
  vTaskDelete(NULL);
}

// Doom Hook
int p4_doom_get_key(int *pressed, unsigned char *key) {
  doom_key_event_t ev;
  // Only physical USB keyboard
  if (xQueueReceive(doom_key_queue, &ev, 0) == pdTRUE) {
    *pressed = ev.pressed;
    *key = ev.key;
    return 1;
  }
  return 0; // Empty
}

#include "doomEsp_sound.h"

void doomEsp_Start(bsp_p4_handles_t bsp_handles, uint16_t *frame_buffer) {
  ESP_LOGI(TAG, "Starting Ultimate Doom on ESP32-P4 with USB Keyboard support...");

  global_frame_buffer = frame_buffer;
  g_bsp_handles = bsp_handles;

  /* Logical landscape 1280×720; after rotation width and height swap.
   * Quantize before calculating offsets so the result is truly centered. */
  {
    float sx = (float)LCD_V_RES / (float)DOOM_W;
    float sy = (float)LCD_H_RES / (float)DOOM_H;
    float fit = sx < sy ? sx : sy;
    g_scale = (int)(fit * 16.0f) / 16.0f;
    g_scaled_w = (int)(DOOM_W * g_scale);
    g_scaled_h = (int)(DOOM_H * g_scale);
    g_out_x = (LCD_H_RES - g_scaled_h) / 2;
    g_out_y = (LCD_V_RES - g_scaled_w) / 2;
    ESP_LOGI(TAG, "Landscape %dx%d | DOOM %dx%d, rotation=90, physical offset=(%d,%d)",
             LCD_V_RES, LCD_H_RES, g_scaled_w, g_scaled_h, g_out_x, g_out_y);
  }
  /* Fill entire framebuffer with black so letterbox areas stay dark. */
  memset(global_frame_buffer, 0, LCD_H_RES * LCD_V_RES * 2);
  esp_cache_msync(global_frame_buffer, LCD_H_RES * LCD_V_RES * 2,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M);

  // 0. Keyboard Queue
  doom_key_queue = xQueueCreate(32, sizeof(doom_key_event_t));
  assert(doom_key_queue);
  BaseType_t keyboard_created = xTaskCreatePinnedToCore(
      tab5_keyboard_task, "tab5_keyboard", 4096, NULL, 2, NULL, 0);
  assert(keyboard_created == pdPASS);

  // 2. Framebuffer Queue and Hardware PPA
  doom_rb565 = (uint16_t *)heap_caps_aligned_alloc(64, DOOM_W * DOOM_H * 2,
                                                   MALLOC_CAP_INTERNAL);

  ppa_client_config_t ppa_config = {.oper_type = PPA_OPERATION_SRM};
  ESP_ERROR_CHECK(ppa_register_client(&ppa_config, &ppa_client));

  // 3. Initialize USB HOST Hardware (HID)
  ESP_LOGI(TAG, "Initializing USB Host Driver...");
  BaseType_t task_created =
      xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
                              xTaskGetCurrentTaskHandle(), 2, NULL, 0);
  assert(task_created == pdTRUE);
  ulTaskNotifyTake(false, 1000); // Wait for task to initialize usb_host

  const hid_host_driver_config_t hid_host_driver_config = {
      .create_background_task = true,
      .task_priority = 5,
      .stack_size = 4096,
      .core_id = 0,
      .callback = hid_host_device_callback,
      .callback_arg = NULL};
  ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

  // Initialize Sound Engine
  doomEsp_SoundInit();

  // 3.5 Load Ultimate Doom from the microSD card.
  char *iwad_path = NULL;
  char *pwad_path = NULL;                // optional PWAD

  if (bsp_sdcard_mount() == ESP_OK) {
    ESP_LOGI(TAG,
             "SD Card mounted successfully at /sdcard. Searching for DOOM-ultimate.wad...");
    chdir("/sdcard");
    strcpy(doomEsp_savedir, "/sdcard/");
    static char *const sd_iwads[] = {
        "/sdcard/doom/DOOM-ultimate.wad", "/sdcard/doom/DOOM-ULTIMATE.WAD",
        "/sdcard/DOOM-ultimate.wad", "/sdcard/DOOM-ULTIMATE.WAD",
    };
    bool found_sd_iwad = false;
    bool in_doom_folder = false;
    for (unsigned i = 0; i < sizeof(sd_iwads) / sizeof(sd_iwads[0]); i++) {
      FILE *candidate = fopen(sd_iwads[i], "rb");
      if (!candidate) continue;
      fclose(candidate);
      iwad_path = sd_iwads[i];
      found_sd_iwad = true;
      in_doom_folder = i < 2;
      ESP_LOGI(TAG, "Found SD IWAD: %s", iwad_path);
      break;
    }
    if (!found_sd_iwad) {
      ESP_LOGE(TAG, "DOOM-ultimate.wad not found. Place it in /doom or the root of the microSD card.");
      return;
    } else {
      // Keep optional mods alongside the selected SD IWAD.
      char *const mods_folder[] = {"/sdcard/doom/chiquito.wad", "/sdcard/doom/CHIQUITO.WAD"};
      char *const mods_root[] = {"/sdcard/chiquito.wad", "/sdcard/CHIQUITO.WAD"};
      char *const *mods = in_doom_folder ? mods_folder : mods_root;
      for (unsigned i = 0; i < 2; i++) {
        FILE *candidate = fopen(mods[i], "rb");
        if (!candidate) continue;
        fclose(candidate);
        pwad_path = mods[i];
        ESP_LOGI(TAG, "Found PWAD: %s", pwad_path);
        break;
      }
      if (in_doom_folder) {
        chdir("/sdcard/doom");
        strcpy(doomEsp_savedir, "/sdcard/doom/");
      }
    }
  } else {
    ESP_LOGE(
        TAG,
        "Could not mount the microSD card. Ultimate Doom requires DOOM-ultimate.wad on microSD.");
    return;
  }

  if (!iwad_path) {
    ESP_LOGE(TAG, "No readable DOOM-ultimate.wad found on microSD");
    return;
  }
  ESP_LOGI(TAG, "Starting Ultimate Doom. IWAD: %s, PWAD: %s", iwad_path,
           pwad_path ? pwad_path : "None");

  // 4. Boot DOOM
  if (pwad_path) {
    char *doom_argv[] = {"doom",    "-iwad",    iwad_path, "-file",
                         pwad_path, "-gfxmode", "rgb565"};
    doomgeneric_Create(7, doom_argv);
  } else {
    char *doom_argv[] = {"doom", "-iwad", iwad_path, "-gfxmode", "rgb565"};
    doomgeneric_Create(5, doom_argv);
  }

  while (1) {
    doomgeneric_Tick();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
