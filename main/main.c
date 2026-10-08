/**
 * @file hello_world_main.c
 * @brief Base ESP32-P4 application Launcher
 */

#include "bsp_tab5.h"
#include "doomEsp.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"

static const char *TAG = "APP_MAIN";

static bsp_p4_handles_t bsp_handles;
static uint16_t *frame_buffer;

void app_main(void) {
  ESP_LOGI(TAG, "Iniciando Sistema Base ESP32-P4...");

  // Hardware (Pantalla)
  ESP_ERROR_CHECK(bsp_p4_init_hardware(&bsp_handles));
  void *fb0 = NULL;
  ESP_ERROR_CHECK(
      esp_lcd_dpi_panel_get_frame_buffer(bsp_handles.panel_handle, 1, &fb0));
  frame_buffer = (uint16_t *)fb0;

  // Lanzar DOOM
  doomEsp_Start(bsp_handles, frame_buffer);
}
