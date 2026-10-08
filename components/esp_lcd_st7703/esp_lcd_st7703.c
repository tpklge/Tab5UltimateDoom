/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_st7703.h"
#include "hal/mipi_dsi_host_ll.h"
#include "esp_timer.h"

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save surrent value of LCD_CMD_COLMOD register
    const st7703_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    bool init_in_command_mode;
    bool vendor_cmds_done; // true when vendor init cmds already sent before DPI creation
    struct {
        unsigned int reset_level: 1;
    } flags;
    // To save the original functions of MIPI DPI panel
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} st7703_panel_t;

static const char *TAG = "st7703";

static esp_err_t panel_st7703_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7703_init(esp_lcd_panel_t *panel);

/* Send vendor init commands over the DBI (LP) channel.
 * Must be called while the DSI bridge is NOT yet enabled (before
 * esp_lcd_new_panel_dpi), otherwise the bridge underrun timer blocks LP. */
static esp_err_t st7703_send_vendor_init_cmds(esp_lcd_panel_io_handle_t io,
                                               const st7703_lcd_init_cmd_t *cmds,
                                               uint16_t n)
{
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "pre-DPI cmd[%d]: 0x%02X (%d bytes)", i, cmds[i].cmd, cmds[i].data_bytes);
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, cmds[i].cmd, cmds[i].data,
                                                      cmds[i].data_bytes), TAG, "send command failed");
        int64_t deadline = esp_timer_get_time() + 100000;
        while (!mipi_dsi_host_ll_gen_is_cmd_fifo_empty(MIPI_DSI_LL_GET_HOST(0)) ||
               !mipi_dsi_host_ll_gen_is_write_fifo_empty(MIPI_DSI_LL_GET_HOST(0))) {
            if (esp_timer_get_time() >= deadline) {
                ESP_LOGE(TAG, "cmd 0x%02X did not drain: cmd_empty=%d payload_empty=%d",
                         cmds[i].cmd,
                         mipi_dsi_host_ll_gen_is_cmd_fifo_empty(MIPI_DSI_LL_GET_HOST(0)),
                         mipi_dsi_host_ll_gen_is_write_fifo_empty(MIPI_DSI_LL_GET_HOST(0)));
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(1);
        }
        ESP_LOGI(TAG, "cmd 0x%02X: TX FIFOs drained", cmds[i].cmd);
        uint32_t delay = cmds[i].delay_ms > 0 ? cmds[i].delay_ms : 10;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
    return ESP_OK;
}
static esp_err_t panel_st7703_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7703_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7703_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7703_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

static const st7703_lcd_init_cmd_t vendor_specific_init_default[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xB9, (uint8_t []){0xF1, 0x12, 0x83}, 3, 0},
    {0xB1, (uint8_t []){0x00, 0x00, 0x00, 0xDA, 0x80}, 5, 0},
    {0xB2, (uint8_t []){0xC8, 0x02, 0xF0}, 3, 0},
    {0xB3, (uint8_t []){0x10, 0x10, 0x28, 0x28, 0x03, 0xFF, 0x00, 0x00, 0x00, 0x00}, 10, 0},
    {0xB4, (uint8_t []){0x80}, 1, 0},
    {0xB5, (uint8_t []){0x0A, 0x0A}, 2, 0},
    {0xB6, (uint8_t []){0x8B, 0x8B}, 2, 0},
    {0xB8, (uint8_t []){0x26, 0x22, 0xF0, 0x13}, 4, 0},
    // 0xBA configures DSI lane timing in the panel; give it 50ms to settle
    // before sending further commands or the command FIFO stalls.
    {0xBA, (uint8_t []){0x31, 0x81, 0x05, 0xF9, 0x0E, 0x0E, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x25, 0x00, 0x90, 0x0A, 0x00, 0x00, 0x01, 0x4F, 0x01, 0x00, 0x00, 0x37}, 27, 50},
    {0xBC, (uint8_t []){0x47}, 1, 0},
    {0xBF, (uint8_t []){0x02, 0x11, 0x00}, 3, 0},
    {0xC0, (uint8_t []){0x73, 0x73, 0x50, 0x50, 0x00, 0x00, 0x12, 0x70, 0x00}, 9, 0},
    {0xC1, (uint8_t []){0x54, 0xC0, 0x32, 0x32, 0x77, 0xF1, 0xFF, 0xFF, 0xCC, 0xCC, 0x77, 0x77}, 12, 0},
    {0xC6, (uint8_t []){0x82, 0x00, 0xBF, 0xFF, 0x00, 0xFF}, 6, 0},
    {0xC7, (uint8_t []){0xB8, 0x00, 0x0A, 0x00, 0x00, 0x02}, 6, 0},
    // 0xC8 omitted: this command changes panel DSI receive state and causes
    // subsequent LP transactions to hang on the command FIFO indefinitely.
    {0xCC, (uint8_t []){0x0B}, 1, 0},
    // 0xE0 (34 bytes) omitted: payload exceeds the 32-byte DSI TX FIFO depth;
    // writing it deadlocks the hardware before the command header can be sent.
    {0xE3, (uint8_t []){0x07, 0x07, 0x0B, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xC0, 0x10}, 14, 0},
    // 0xE9 (63 bytes) omitted: payload exceeds 32-byte DSI TX FIFO depth.
    // 0xEA (63 bytes) omitted: payload exceeds 32-byte DSI TX FIFO depth.
    {0xEF, (uint8_t []){0xFF, 0xFF, 0x01}, 3, 0},
    // Standard DCS configuration is transmitted before DPI, with ACK disabled.
    {0x36, (uint8_t []){0x00}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0x11, NULL, 0, 250},
    {0x29, NULL, 0, 20},

};

esp_err_t esp_lcd_new_panel_st7703(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    ESP_LOGI(TAG, "version: %d.%d.%d", ESP_LCD_ST7703_VER_MAJOR, ESP_LCD_ST7703_VER_MINOR,
             ESP_LCD_ST7703_VER_PATCH);
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    st7703_vendor_config_t *vendor_config = (st7703_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->mipi_config.dpi_config && vendor_config->mipi_config.dsi_bus, ESP_ERR_INVALID_ARG, TAG,
                        "invalid vendor config");

    esp_err_t ret = ESP_OK;
    st7703_panel_t *st7703 = (st7703_panel_t *)calloc(1, sizeof(st7703_panel_t));
    ESP_RETURN_ON_FALSE(st7703, ESP_ERR_NO_MEM, TAG, "no mem for st7703 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        st7703->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        st7703->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        st7703->colmod_val = 0x55;
        break;
    case 18: // RGB666
        st7703->colmod_val = 0x66;
        break;
    case 24: // RGB888
        st7703->colmod_val = 0x77;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st7703->io = io;
    st7703->init_cmds = vendor_config->init_cmds;
    st7703->init_cmds_size = vendor_config->init_cmds_size;
    st7703->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7703->flags.reset_level = panel_dev_config->flags.reset_active_high;
    st7703->init_in_command_mode = vendor_config->init_in_command_mode;
    st7703->vendor_cmds_done = false;

    // When init_in_command_mode=true, send vendor init commands HERE — before
    // esp_lcd_new_panel_dpi() — because creating the DPI panel enables the
    // DSI bridge, which starts an internal frame timer. After ~18 frame periods
    // (~320ms at 58MHz/804x1312) with no video data the bridge enters underrun
    // state and blocks LP command FIFO drain, hanging every subsequent command.
    if (vendor_config->init_in_command_mode) {
        // IDF 5.4 DBI enables ACK requests with no response timeout. Test
        // unacknowledged writes and verify actual FIFO drain after each command.
        mipi_dsi_host_ll_enable_cmd_ack(MIPI_DSI_LL_GET_HOST(0), false);
        ESP_LOGI(TAG, "Tab5 diagnostic: DCS command ACK disabled");
        const st7703_lcd_init_cmd_t *cmds = vendor_config->init_cmds ?
                                             vendor_config->init_cmds :
                                             vendor_specific_init_default;
        uint16_t n = vendor_config->init_cmds ?
                     vendor_config->init_cmds_size :
                     (sizeof(vendor_specific_init_default) / sizeof(st7703_lcd_init_cmd_t));
        ESP_LOGI(TAG, "sending %d vendor init cmds before DPI panel creation", n);
        ESP_GOTO_ON_ERROR(st7703_send_vendor_init_cmds(io, cmds, n), err, TAG,
                          "send pre-DPI vendor init commands failed");
        st7703->vendor_cmds_done = true;
        ESP_LOGI(TAG, "pre-DPI vendor init cmds done");
    }

    // Create MIPI DPI panel (enables DSI bridge — must be after init cmds above)
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_LOGI(TAG, "calling esp_lcd_new_panel_dpi...");
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus, vendor_config->mipi_config.dpi_config, &panel_handle), err, TAG,
                      "create MIPI DPI panel failed");
    ESP_LOGI(TAG, "esp_lcd_new_panel_dpi OK @%p", panel_handle);

    // Save the original functions of MIPI DPI panel
    st7703->del = panel_handle->del;
    st7703->init = panel_handle->init;
    // Overwrite the functions of MIPI DPI panel
    panel_handle->del = panel_st7703_del;
    panel_handle->init = panel_st7703_init;
    panel_handle->reset = panel_st7703_reset;
    panel_handle->mirror = panel_st7703_mirror;
    panel_handle->invert_color = panel_st7703_invert_color;
    panel_handle->disp_on_off = panel_st7703_disp_on_off;
    panel_handle->user_data = st7703;
    *ret_panel = panel_handle;
    ESP_LOGD(TAG, "new st7703 panel @%p", st7703);

    return ESP_OK;

err:
    if (st7703) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st7703);
    }
    return ret;
}

static esp_err_t panel_st7703_del(esp_lcd_panel_t *panel)
{
    st7703_panel_t *st7703 = (st7703_panel_t *)panel->user_data;

    // Delete MIPI DPI panel
    ESP_RETURN_ON_ERROR(st7703->del(panel), TAG, "del st7703 panel failed");
    if (st7703->reset_gpio_num >= 0) {
        gpio_reset_pin(st7703->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del st7703 panel @%p", st7703);
    free(st7703);

    return ESP_OK;
}

static esp_err_t panel_st7703_init(esp_lcd_panel_t *panel)
{
    st7703_panel_t *st7703 = (st7703_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7703->io;
    const st7703_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_cmd_overwritten = false;

    ESP_LOGI(TAG, "panel_st7703_init: start (cmd_mode=%d, vendor_done=%d)",
             st7703->init_in_command_mode, st7703->vendor_cmds_done);

    /* ID read via BTA skipped: the HAL busy-loops forever if the panel
     * does not support BTA (Bus Turn Around) in DPI video mode. */

    if (st7703->vendor_cmds_done) {
        // Commands, including Display On, were transmitted before DPI creation.
        ESP_LOGI(TAG, "panel_st7703_init: starting DPI (vendor cmds already done)");
        ESP_RETURN_ON_ERROR(st7703->init(panel), TAG, "init MIPI DPI panel failed");
        ESP_LOGI(TAG, "panel_st7703_init: DPI started (pre-DPI commands complete)");
    } else if (!st7703->init_in_command_mode) {
        // Non-cmd-mode: start DPI first, then send cmds during blanking
        ESP_LOGI(TAG, "panel_st7703_init: starting DPI (non-cmd-mode)");
        ESP_RETURN_ON_ERROR(st7703->init(panel), TAG, "init MIPI DPI panel failed");
        ESP_LOGI(TAG, "panel_st7703_init: DPI started");
    }

    if (!st7703->vendor_cmds_done) {
        // MADCTL/COLMOD not yet sent — send them now
        ESP_LOGI(TAG, "panel_st7703_init: sending MADCTL=0x%02X COLMOD=0x%02X",
                 st7703->madctl_val, st7703->colmod_val);
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
            st7703->madctl_val,
        }, 1), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
            st7703->colmod_val,
        }, 1), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        ESP_LOGI(TAG, "panel_st7703_init: MADCTL/COLMOD already sent in pre-DPI sequence, skipping");
    }

    if (!st7703->vendor_cmds_done) {
        // Vendor commands not yet sent — send them now (non-cmd-mode or legacy path)
        if (st7703->init_cmds) {
            init_cmds = st7703->init_cmds;
            init_cmds_size = st7703->init_cmds_size;
        } else {
            init_cmds = vendor_specific_init_default;
            init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(st7703_lcd_init_cmd_t);
        }

        for (int i = 0; i < init_cmds_size; i++) {
            if (init_cmds[i].data_bytes > 0) {
                switch (init_cmds[i].cmd) {
                case LCD_CMD_MADCTL:
                    is_cmd_overwritten = true;
                    st7703->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
                    break;
                case LCD_CMD_COLMOD:
                    is_cmd_overwritten = true;
                    st7703->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
                    break;
                default:
                    is_cmd_overwritten = false;
                    break;
                }
                if (is_cmd_overwritten) {
                    is_cmd_overwritten = false;
                    ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence",
                             init_cmds[i].cmd);
                }
            }
            ESP_LOGI(TAG, "cmd[%d]: 0x%02X (%d bytes)", i, init_cmds[i].cmd, init_cmds[i].data_bytes);
            ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
            uint32_t delay = init_cmds[i].delay_ms > 0 ? init_cmds[i].delay_ms : 10;
            vTaskDelay(pdMS_TO_TICKS(delay));
        }
        ESP_LOGI(TAG, "panel_init: vendor cmds done");
    }

    if (st7703->init_in_command_mode && !st7703->vendor_cmds_done) {
        // Legacy cmd-mode: vendor cmds sent above, now start DPI
        ESP_LOGI(TAG, "panel_st7703_init: starting DPI (cmd-mode legacy)");
        ESP_RETURN_ON_ERROR(st7703->init(panel), TAG, "init MIPI DPI panel failed");
        ESP_LOGI(TAG, "panel_st7703_init: DPI started");
    }

    return ESP_OK;
}

static esp_err_t panel_st7703_reset(esp_lcd_panel_t *panel)
{
    st7703_panel_t *st7703 = (st7703_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7703->io;

    // If vendor init was sent pre-DPI (including Sleep Out), skip reset here.
    // Sending SW_RESET after Sleep Out hangs because the panel occupies the DSI
    // lanes after waking up. HW reset was already done via IO expander earlier.
    if (st7703->vendor_cmds_done) {
        ESP_LOGI(TAG, "panel_st7703_reset: skipped (pre-DPI init already done)");
        return ESP_OK;
    }

    // Perform hardware reset
    if (st7703->reset_gpio_num >= 0) {
        gpio_set_level(st7703->reset_gpio_num, !st7703->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(st7703->reset_gpio_num, st7703->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st7703->reset_gpio_num, !st7703->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else if (io) { // Perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_st7703_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7703_panel_t *st7703 = (st7703_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7703->io;
    uint8_t command = 0;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");

    return ESP_OK;
}

static esp_err_t panel_st7703_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7703_panel_t *st7703 = (st7703_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7703->io;
    uint8_t madctl_val = st7703->madctl_val;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    // Control mirror through LCD command
    if (mirror_x) {
        ESP_LOGW(TAG, "Mirror X is not supported");
    }

    if (mirror_y) {
        madctl_val |= BIT(7);
    } else {
        madctl_val &= ~BIT(7);
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t []) {
        madctl_val
    }, 1), TAG, "send command failed");
    st7703->madctl_val = madctl_val;

    return ESP_OK;
}

static esp_err_t panel_st7703_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7703_panel_t *st7703 = (st7703_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7703->io;

    // The pre-DPI sequence already sent Display On. Do not claim that
    // Display Off succeeded when this path did not send it.
    if (st7703->vendor_cmds_done) {
        if (!on_off) return ESP_ERR_NOT_SUPPORTED;
        ESP_LOGI(TAG, "disp_on_off(1): Display On already transmitted pre-DPI");
        return ESP_OK;
    }

    int command = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
#endif
