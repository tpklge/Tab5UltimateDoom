/**
 * @file bsp_tab5.c
 * @brief Hardware initialization for M5Stack Tab5.
 *
 * Replaces bsp_p4_eval.c.  Same public API, completely different hardware.
 *
 * Display pipeline:
 *   ESP32-P4 MIPI-DSI (2 lanes, 965 Mbps)
 *     → ST7123 controller (720×1280 portrait)
 *   The DPI framebuffer is kept in portrait orientation.
 *   DOOM's 320×200 output is scaled and letterboxed by the PPA in doomEsp.c.
 *
 * Audio pipeline:
 *   ES8388 codec (I2C @ 0x10, I2S on GPIOs 27-30)
 *   Speaker amp NS4150B enabled via PI4IOE5V6408 IO expander bit P1.
 *
 * This unit reports TDDI firmware version 3 at I2C 0x55 (ST7123).
 * See TAB5DOOM_DISPLAY_INIT_HISTORY.txt for hardware validation.
 */

#include "bsp_tab5.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/sdmmc_host.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_io_expander.h"
#include "esp_io_expander_pi4ioe5v6408.h"
#include "esp_lcd_st7123.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP_TAB5";

/* Shared handles kept as file-statics so audio can reuse the I2C bus. */
static i2c_master_bus_handle_t  s_i2c_bus    = NULL;
static esp_io_expander_handle_t s_ioexp      = NULL;
static i2s_chan_handle_t        s_i2s_tx     = NULL;
static i2s_chan_handle_t        s_i2s_rx     = NULL;

/* Enable USB-A VBUS on expander B P3 without resetting its other outputs. */
static esp_err_t enable_usb_host_power(void)
{
    i2c_master_dev_handle_t device = NULL;
    const i2c_device_config_t cfg = {
        .device_address = 0x44, .scl_speed_hz = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus, &cfg, &device);
    if (ret != ESP_OK) return ret;
    const uint8_t registers[] = {0x05, 0x07, 0x03}; // output, high-Z, direction
    for (unsigned i = 0; i < sizeof(registers); i++) {
        uint8_t value;
        ret = i2c_master_transmit_receive(device, &registers[i], 1, &value, 1, 100);
        if (ret != ESP_OK) break;
        value = registers[i] == 0x07 ? value & ~BIT(3) : value | BIT(3);
        uint8_t write[] = {registers[i], value};
        ret = i2c_master_transmit(device, write, sizeof(write), 100);
        if (ret != ESP_OK) break;
    }
    esp_err_t cleanup = i2c_master_bus_rm_device(device);
    if (ret == ESP_OK) ret = cleanup;
    if (ret == ESP_OK) ESP_LOGI(TAG, "USB-A 5V enabled (expander 0x44 P3)");
    return ret;
}

esp_err_t bsp_audio_configure_output(void)
{
    i2c_master_dev_handle_t device = NULL;
    const i2c_device_config_t cfg = {
        .device_address = 0x10, .scl_speed_hz = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus, &cfg, &device);
    if (ret != ESP_OK) return ret;
    // M5Unified Tab5 DAC routing; our I2S retains its 256x MCLK ratio.
    static const uint8_t regs[][2] = {
        {0x00, 0x80}, {0x00, 0x00}, {0x00, 0x0E}, {0x01, 0x00},
        {0x02, 0x0A}, {0x03, 0xFF}, {0x04, 0x3C},
        {0x05, 0x00}, {0x06, 0x00}, {0x07, 0x7C}, {0x08, 0x00},
        {0x17, 0x18}, {0x18, 0x02}, {0x19, 0x20},
        {0x1C, 0x08}, {0x1D, 0x00}, {0x26, 0x00},
        {0x27, 0xB8}, {0x2A, 0xB8}, {0x2B, 0x08},
        {0x2D, 0x00}, {0x2E, 0x21}, {0x2F, 0x21}, {0x30, 0x21}, {0x31, 0x21},
    };
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        ret = i2c_master_transmit(device, regs[i], 2, 100);
        if (ret != ESP_OK) break;
        vTaskDelay(1);
        uint8_t readback;
        ret = i2c_master_transmit_receive(device, regs[i], 1, &readback, 1, 100);
        if (ret != ESP_OK) break;
        ESP_LOGI(TAG, "ES8388 reg 0x%02X=0x%02X", regs[i][0], readback);
    }
    esp_err_t cleanup = i2c_master_bus_rm_device(device);
    if (ret == ESP_OK) ret = cleanup;
    return ret;
}

/* -------------------------------------------------------------------------
 * ST7123 720×1280 timing from the official M5Stack Tab5 BSP.
 * 965 Mbps, 2 lanes; 70 MHz / (802 × 1510) ≈ 57.8 fps.
 * ------------------------------------------------------------------------- */
#define ST7123_H_PULSE               2
#define ST7123_H_BACK                40
#define ST7123_H_FRONT               40
#define ST7123_V_PULSE               2
#define ST7123_V_BACK                8
#define ST7123_V_FRONT               220

/* -------------------------------------------------------------------------
 * Backlight (LEDC PWM on GPIO 22)
 * ------------------------------------------------------------------------- */
static esp_err_t init_backlight(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_12_BIT,
        .timer_num       = LEDC_TIMER_1,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const ledc_channel_config_t chan = {
        .gpio_num   = LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_1,
        .duty       = 4095,   /* 100% brightness */
        .hpoint     = 0,
    };
    return ledc_channel_config(&chan);
}

/* -------------------------------------------------------------------------
 * MIPI DPHY internal LDO (same channel/voltage as EV-Board)
 * ------------------------------------------------------------------------- */
static esp_err_t enable_dsi_phy_power(esp_ldo_channel_handle_t *chan)
{
    esp_ldo_channel_config_t cfg = {
        .chan_id    = MIPI_DPHY_LDO_CHAN,
        .voltage_mv = MIPI_DPHY_LDO_VOLTAGE_MV,
    };
    return esp_ldo_acquire_channel(&cfg, chan);
}

/* -------------------------------------------------------------------------
 * Shared I2C bus (touch + codec + IO expander)
 * ------------------------------------------------------------------------- */
static esp_err_t init_i2c(void)
{
    if (s_i2c_bus) return ESP_OK;
    i2c_master_bus_config_t cfg = {
        .clk_source             = I2C_CLK_SRC_DEFAULT,
        .i2c_port               = I2C_NUM_0,
        .scl_io_num             = BSP_I2C_SCL,
        .sda_io_num             = BSP_I2C_SDA,
        .glitch_ignore_cnt      = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

/* -------------------------------------------------------------------------
 * IO Expander PI4IOE5V6408 (0x43)
 * P1 = speaker amp enable (set high after codec init)
 * P6 = used by touch reset pulse (toggled in hardware init sequence below)
 * ------------------------------------------------------------------------- */
static esp_err_t init_io_expander(void)
{
    if (s_ioexp) return ESP_OK;
    ESP_ERROR_CHECK(init_i2c());

    ESP_ERROR_CHECK(esp_io_expander_new_i2c_pi4ioe5v6408(s_i2c_bus, BSP_IOEXP_I2C_ADDR, &s_ioexp));
    if (!s_ioexp) {
        ESP_LOGE(TAG, "Failed to init IO expander at 0x%02X", BSP_IOEXP_I2C_ADDR);
        return ESP_FAIL;
    }
    /* Bits 1,2,4,6 as outputs; start: 5V off, LCD/TP reset held low, SPK off. */
    uint64_t out_bits = BIT64(BSP_IOEXP_SPK_BIT) | BIT64(BSP_IOEXP_5V_BIT) |
                        BIT64(BSP_IOEXP_LCD_RST_BIT) | BIT64(BSP_IOEXP_TP_RST_BIT);
    ESP_ERROR_CHECK(esp_io_expander_set_dir(s_ioexp, out_bits, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_ioexp, out_bits, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_pullupdown(s_ioexp,
        BIT64(BSP_IOEXP_LCD_RST_BIT) | BIT64(BSP_IOEXP_TP_RST_BIT), IO_EXPANDER_PULL_UP));
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Main hardware init (display + touch)
 * ------------------------------------------------------------------------- */
esp_err_t bsp_p4_init_hardware(bsp_p4_handles_t *handles)
{
    if (!handles) return ESP_ERR_INVALID_ARG;
    esp_err_t ret;

    /* 1. Internal LDO for MIPI DPHY */
    ret = enable_dsi_phy_power(&handles->ldo_handle);
    if (ret != ESP_OK) return ret;

    /* 2. I2C bus */
    ret = init_i2c();
    if (ret != ESP_OK) return ret;
    handles->i2c_bus = s_i2c_bus;

    /* 3. IO expander */
    ret = init_io_expander();
    if (ret != ESP_OK) return ret;
    ret = enable_usb_host_power();
    if (ret != ESP_OK) return ret;

    /* 4. Enable external 5V rail (bit 2) before display/touch init */
    esp_io_expander_set_level(s_ioexp, BIT64(BSP_IOEXP_5V_BIT), 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 5. LCD + touch reset: bit 4 (LCD) and bit 6 (touch)
     *    Low for 10 ms → high, wait 120 ms for panel to boot */
    esp_io_expander_set_level(s_ioexp,
        BIT64(BSP_IOEXP_LCD_RST_BIT) | BIT64(BSP_IOEXP_TP_RST_BIT), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    /* P4 LCD reset is released through a pull-up, matching M5Stack's BSP.
     * Keep its output latch low and switch direction rather than driving high. */
    ESP_ERROR_CHECK(esp_io_expander_set_dir(s_ioexp,
        BIT64(BSP_IOEXP_LCD_RST_BIT), IO_EXPANDER_INPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_ioexp,
        BIT64(BSP_IOEXP_TP_RST_BIT), 1));
    ESP_LOGI(TAG, "Reset released: LCD=P4 input/pull-up, touch=P6 high");
    vTaskDelay(pdMS_TO_TICKS(120));

    /* I2C scan after reset to show which devices are now visible */
    ESP_LOGI(TAG, "I2C bus scan (after reset):");
    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t probe = i2c_master_probe(s_i2c_bus, addr, 10);
        if (probe == ESP_OK) {
            ESP_LOGI(TAG, "  Found I2C device at 0x%02X", addr);
        }
    }

    /* 5. Backlight PWM */
    if (i2c_master_probe(s_i2c_bus, 0x55, 50) == ESP_OK) {
        i2c_master_dev_handle_t tddi = NULL;
        i2c_device_config_t cfg = {
            .device_address = 0x55, .scl_speed_hz = 100000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &cfg, &tddi));
        uint8_t reg[] = {0x00, 0x00};
        uint8_t version = 0;
        esp_err_t result = i2c_master_transmit_receive(tddi, reg, sizeof(reg),
                                                     &version, 1, 100);
        ESP_LOGI(TAG, "TDDI 0x55 firmware version=%u, read=%s (1=ST7121, 3=ST7123)",
                 version, esp_err_to_name(result));
        ESP_ERROR_CHECK(i2c_master_bus_rm_device(tddi));
    }
    ret = init_backlight();
    if (ret != ESP_OK) return ret;

    /* 6. MIPI DSI bus */
    ESP_LOGI(TAG, "Configuring MIPI DSI bus at %d Mbps...", LCD_BITRATE_MBPS);
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = LCD_DSI_LANES,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = LCD_BITRATE_MBPS,
    };
    ret = esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus);
    if (ret != ESP_OK) return ret;

    /* 7. DBI IO (sends init commands over DSI) */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel  = 0,
        .lcd_cmd_bits     = 8,
        .lcd_param_bits   = 8,
    };
    ret = esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io_handle);
    if (ret != ESP_OK) return ret;
    handles->io_handle = io_handle;

    /* 8. ST7123 panel + DPI timing */
    ESP_LOGI(TAG, "Initializing identified ST7123 display (720x1280, DSI 965Mbps)...");
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel  = 0,
        .dpi_clk_src      = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 70,  /* Official M5Stack ST7123 timing */
        .pixel_format     = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs          = 1,
        .video_timing     = {
            .h_size           = LCD_H_RES,
            .v_size           = LCD_V_RES,
            .hsync_back_porch = ST7123_H_BACK,
            .hsync_pulse_width= ST7123_H_PULSE,
            .hsync_front_porch= ST7123_H_FRONT,
            .vsync_back_porch = ST7123_V_BACK,
            .vsync_pulse_width= ST7123_V_PULSE,
            .vsync_front_porch= ST7123_V_FRONT,
        },
    };

    st7123_vendor_config_t st7123_vendor_cfg = {
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };

    esp_lcd_panel_dev_config_t lcd_cfg = {
        .bits_per_pixel  = 16,
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB,
        .reset_gpio_num  = LCD_RESET_GPIO,  /* GPIO_NUM_NC – reset done via ioexp */
        .vendor_config   = &st7123_vendor_cfg,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ret = esp_lcd_new_panel_st7123(io_handle, &lcd_cfg, &panel);
    if (ret != ESP_OK) return ret;

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    handles->panel_handle = panel;

    /* 9. ST7123 integrated touch at 0x55 is identified but not initialized yet. */
    ESP_LOGI(TAG, "Skipping ST7123 touch init (display bring-up only)");
    handles->touch_handle = NULL;

    ESP_LOGI(TAG, "Tab5 display ready (touch skipped).");
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Audio: I2S bus for ES8388
 * ------------------------------------------------------------------------- */
void bsp_audio_init(void *arg)
{
    ESP_LOGI(TAG, "Initializing I2S for ES8388 (Tab5)...");

    /* Ensure I2C is up (needed for codec control later) */
    init_i2c();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(11025),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_BCLK,
            .ws   = BSP_I2S_WS,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));
}

/* -------------------------------------------------------------------------
 * Audio: ES8388 codec speaker output
 * ------------------------------------------------------------------------- */
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    ESP_LOGI(TAG, "Initializing ES8388 codec...");
    if (!s_i2c_bus) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return NULL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = I2C_NUM_0,
        .addr       = ES8388_CODEC_DEFAULT_ADDR,   /* 0x10 */
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = I2S_NUM_0,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    /* ES8388 has no direct PA GPIO – speaker amp is enabled via IO expander. */
    es8388_codec_cfg_t es8388_cfg = {
        .ctrl_if      = ctrl_if,
        .gpio_if      = gpio_if,
        .codec_mode   = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin       = GPIO_NUM_NC,
        .pa_reverted  = false,
        .master_mode  = false,
        .hw_gain      = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
    };
    const audio_codec_if_t *codec_if = es8388_codec_new(&es8388_cfg);

    /* Enable NS4150B speaker amp via IO expander P1 */
    if (s_ioexp) {
        ESP_ERROR_CHECK(esp_io_expander_set_output_mode(s_ioexp,
            BIT64(BSP_IOEXP_SPK_BIT), IO_EXPANDER_OUTPUT_MODE_PUSH_PULL));
        ESP_ERROR_CHECK(esp_io_expander_set_level(s_ioexp, BIT64(BSP_IOEXP_SPK_BIT), 1));
        uint32_t level = 0;
        ESP_ERROR_CHECK(esp_io_expander_get_level(s_ioexp, BIT64(BSP_IOEXP_SPK_BIT), &level));
        ESP_LOGI(TAG, "Speaker amp P1 push-pull enabled, input level=0x%lx", (unsigned long)level);
    } else {
        ESP_LOGW(TAG, "IO expander not initialized – speaker amp may stay muted");
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    return esp_codec_dev_new(&dev_cfg);
}

/* -------------------------------------------------------------------------
 * SD card
 * ------------------------------------------------------------------------- */
esp_err_t bsp_sdcard_mount(void)
{
    ESP_LOGI(TAG, "Mounting SD card...");

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 64 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    /* SD card power via LDO channel 4 @ 3.3 V (same as EV-Board) */
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = BSP_SD_LDO_CHAN,
        .voltage_mv = BSP_SD_LDO_VOLTAGE_MV,
    };
    esp_ldo_channel_handle_t sd_ldo = NULL;
    if (esp_ldo_acquire_channel(&ldo_cfg, &sd_ldo) != ESP_OK) {
        ESP_LOGW(TAG, "Could not acquire SD LDO – continuing without it");
    }

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk   = GPIO_NUM_43;
    slot.cmd   = GPIO_NUM_44;
    slot.d0    = GPIO_NUM_39;
    slot.d1    = GPIO_NUM_40;
    slot.d2    = GPIO_NUM_41;
    slot.d3    = GPIO_NUM_42;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot,
                                             &mount_cfg, &card);
    if (ret != ESP_OK && sd_ldo) {
        esp_ldo_release_channel(sd_ldo);
    }
    return ret;
}
