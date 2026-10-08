/**
 * @file bsp_tab5.h
 * @brief Hardware abstraction layer for M5Stack Tab5.
 *
 * Replaces bsp_p4_eval.h from the ESP32-P4-Function-EV-Board target.
 * Maintains the same struct and function API so main.c, doomEsp.c and
 * doomEsp_sound.c need only an include path change.
 *
 * Tab5 key differences from the EV-Board:
 *   - Display:  ST7123 (720×1280 portrait), MIPI-DSI 2-lane
 *   - Backlight: GPIO 22 (was GPIO 26)
 *   - Audio:    ES8388 codec @ I2C 0x10  (was ES8311 @ 0x18)
 *   - Speaker amp: NS4150B enabled via PI4IOE5V6408 IO expander (not GPIO 53)
 *   - I2S pins:  MCLK=30, BCLK=27, WS=29, DOUT=26, DIN=28
 *   - I2C pins:  SCL=32, SDA=31  (was 8/7)
 *   - Touch:    ST7123 @ I2C 0x55, INT=GPIO 23
 *   - SD card:  same GPIOs 39-44
 */

#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"
#include "esp_ldo_regulator.h"
#include "esp_vfs_fat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Display ------------------------------------------------------------ */

/* Tab5 display is physically 720×1280 portrait.
 * DOOM runs in this portrait framebuffer; the PPA scales 320×200
 * to 720×450 and centers it with black letterbox bars (415 px top/bottom). */
#define LCD_H_RES               720
#define LCD_V_RES               1280

/* MIPI-DSI: 2 data lanes at 965 Mbps (identified ST7123 panel). */
#define LCD_BITRATE_MBPS        965
#define LCD_DSI_LANES           2

/* GPIO assignments */
#define LCD_BACKLIGHT_GPIO      GPIO_NUM_22   /* LEDC PWM backlight */
#define LCD_RESET_GPIO          GPIO_NUM_NC   /* Reset via IO expander, not GPIO */

/* ---- I2C (shared by touch, codec, IO expander) -------------------------- */
#define BSP_I2C_SCL             GPIO_NUM_32
#define BSP_I2C_SDA             GPIO_NUM_31

/* ---- Touch -------------------------------------------------------------- */
#define BSP_TOUCH_INT           GPIO_NUM_23

/* ---- I2S (ES8388 codec) ------------------------------------------------- */
#define BSP_I2S_MCLK            GPIO_NUM_30
#define BSP_I2S_BCLK            GPIO_NUM_27
#define BSP_I2S_WS              GPIO_NUM_29
#define BSP_I2S_DOUT            GPIO_NUM_26
#define BSP_I2S_DIN             GPIO_NUM_28

/* ---- IO Expander A (PI4IOE5V6408 @ 0x43) -------------------------------- */
#define BSP_IOEXP_I2C_ADDR      0x43
#define BSP_IOEXP_SPK_BIT       1   /* bit 1 = NS4150B speaker amp enable */
#define BSP_IOEXP_5V_BIT        2   /* bit 2 = external 5V rail enable */
#define BSP_IOEXP_LCD_RST_BIT   4   /* bit 4 = LCD reset (active low) */
#define BSP_IOEXP_TP_RST_BIT    6   /* bit 6 = touch reset (active low); P5 is camera reset */

esp_err_t bsp_audio_configure_output(void);

/* ---- MIPI DPHY power (same channel as EV-Board) ------------------------- */
#define MIPI_DPHY_LDO_CHAN      3
#define MIPI_DPHY_LDO_VOLTAGE_MV 2500

/* ---- SD card (same GPIO mapping as EV-Board) ---------------------------- */
#define BSP_SD_LDO_CHAN         4
#define BSP_SD_LDO_VOLTAGE_MV   3300

/* ---- Handle struct (same as bsp_p4_eval.h) ------------------------------ */
typedef struct {
    esp_lcd_panel_handle_t    panel_handle;
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_touch_handle_t    touch_handle;
    esp_ldo_channel_handle_t  ldo_handle;
    i2c_master_bus_handle_t   i2c_bus;
} bsp_p4_handles_t;

/* ---- Public API (same signatures as bsp_p4_eval.h) --------------------- */

/** Initialize display, touch and power management. */
esp_err_t bsp_p4_init_hardware(bsp_p4_handles_t *handles);

/** Mount SD card at /sdcard (optional; falls back to SPIFFS if absent). */
esp_err_t bsp_sdcard_mount(void);

/** Initialize I2S bus for the ES8388 codec. */
void bsp_audio_init(void *arg);

/** Initialize ES8388 codec and return an esp_codec_dev speaker handle. */
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);

#ifdef __cplusplus
}
#endif
