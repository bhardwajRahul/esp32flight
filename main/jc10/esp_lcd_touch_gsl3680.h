/*
 * Silead GSL3680 capacitive touch controller for esp_lcd_touch.
 *
 * Clean-room driver written from the controller's register protocol as
 * used by every public GSLx680 host driver: a firmware/config table is
 * uploaded over I2C after reset (page register 0xF0, 32-bit words), the
 * core is started by writing 0 to 0xE0, and touch points are read from
 * register 0x80 (finger count + up to five 4-byte X/Y records). No code
 * from the GPL Linux gslX680 driver or from vendor demos is used; the
 * firmware table itself is vendor data and lives in gsl3680_fw.inc.
 */
#pragma once

#include "esp_lcd_touch.h"
#include "esp_lcd_panel_io.h"

#define ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS   0x40

#define ESP_LCD_TOUCH_IO_I2C_GSL3680_CONFIG()          \
    {                                                  \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS, \
        .control_phase_bytes = 1,                      \
        .dc_bit_offset = 0,                            \
        .lcd_cmd_bits = 8,                             \
        .flags = { .disable_control_phase = 1 },       \
    }

esp_err_t esp_lcd_touch_new_i2c_gsl3680(const esp_lcd_panel_io_handle_t io,
                                        const esp_lcd_touch_config_t *config,
                                        esp_lcd_touch_handle_t *out_touch);
