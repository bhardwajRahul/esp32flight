/* See esp_lcd_touch_gsl3680.h for provenance. */
#include "esp_lcd_touch_gsl3680.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gsl3680_fw.inc"

static const char *TAG = "gsl3680";

#define GSL_REG_TOUCH      0x80   /* finger count byte + point records */
#define GSL_REG_CTRL       0xE0   /* 0x88 = hold in reset, 0x00 = run */
#define GSL_REG_CLOCK      0xE4   /* 0x04 = clock on */
#define GSL_REG_POWER      0xBC   /* 4 zero bytes = power all blocks */
#define GSL_REG_PAGE       0xF0   /* firmware page select */
#define GSL_REG_SOFTRST    0x88
#define GSL_MAX_POINTS     5
#define GSL_TOUCH_LEN      (4 + GSL_MAX_POINTS * 4)

static esp_err_t gsl_write(esp_lcd_touch_handle_t tp, uint8_t reg, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(tp->io, reg, data, len);
}

static esp_err_t gsl_read(esp_lcd_touch_handle_t tp, uint8_t reg, uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_rx_param(tp->io, reg, data, len);
}

static esp_err_t gsl_write_u8(esp_lcd_touch_handle_t tp, uint8_t reg, uint8_t v)
{
    return gsl_write(tp, reg, &v, 1);
}

static esp_err_t gsl_write_u32(esp_lcd_touch_handle_t tp, uint8_t reg, uint32_t v)
{
    uint8_t b[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF };
    return gsl_write(tp, reg, b, 4);
}

/* Hardware reset pulse plus the register dance that wakes the core. */
static esp_err_t gsl_reset(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_ERROR(gsl_write_u8(tp, GSL_REG_CTRL, 0x88), TAG, "hold reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gsl_write_u8(tp, GSL_REG_CLOCK, 0x04), TAG, "clock on");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gsl_write_u32(tp, GSL_REG_POWER, 0), TAG, "power on");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t gsl_clear(esp_lcd_touch_handle_t tp)
{
    ESP_RETURN_ON_ERROR(gsl_write_u8(tp, GSL_REG_CTRL, 0x88), TAG, "hold");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(gsl_write_u8(tp, GSL_REG_SOFTRST, 0x01), TAG, "softrst");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(gsl_write_u8(tp, GSL_REG_CLOCK, 0x04), TAG, "clock");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(gsl_write_u8(tp, GSL_REG_CTRL, 0x00), TAG, "run");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static esp_err_t gsl_load_fw(esp_lcd_touch_handle_t tp)
{
    size_t n = sizeof(k_gsl3680_fw) / sizeof(k_gsl3680_fw[0]);
    for (size_t i = 0; i < n; i++) {
        const gsl3680_fw_row_t *r = &k_gsl3680_fw[i];
        esp_err_t err = (r->reg == GSL_REG_PAGE)
            ? gsl_write_u8(tp, r->reg, (uint8_t)r->val)
            : gsl_write_u32(tp, r->reg, r->val);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fw upload failed at row %u/%u", (unsigned)i, (unsigned)n);
            return err;
        }
    }
    ESP_LOGI(TAG, "firmware uploaded (%u rows)", (unsigned)n);
    return ESP_OK;
}

static esp_err_t gsl_start(esp_lcd_touch_handle_t tp)
{
    ESP_RETURN_ON_ERROR(gsl_write_u8(tp, GSL_REG_CTRL, 0x00), TAG, "start core");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t gsl_bringup(esp_lcd_touch_handle_t tp)
{
    /* clear -> reset -> upload -> start -> reset -> start: the sequence
       every working host driver performs; the second reset/start pair
       makes the freshly loaded firmware take effect */
    ESP_RETURN_ON_ERROR(gsl_clear(tp), TAG, "clear");
    ESP_RETURN_ON_ERROR(gsl_reset(tp), TAG, "reset");
    ESP_RETURN_ON_ERROR(gsl_load_fw(tp), TAG, "load");
    ESP_RETURN_ON_ERROR(gsl_start(tp), TAG, "start");
    ESP_RETURN_ON_ERROR(gsl_reset(tp), TAG, "reset2");
    ESP_RETURN_ON_ERROR(gsl_start(tp), TAG, "start2");
    return ESP_OK;
}

static esp_err_t gsl_read_data(esp_lcd_touch_handle_t tp)
{
    uint8_t buf[GSL_TOUCH_LEN];
    ESP_RETURN_ON_ERROR(gsl_read(tp, GSL_REG_TOUCH, buf, sizeof(buf)), TAG, "read");

    uint8_t cnt = buf[0];
    if (cnt > GSL_MAX_POINTS) {
        cnt = 0;   /* garbage while the core is settling */
    }
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = cnt;
    for (uint8_t i = 0; i < cnt; i++) {
        const uint8_t *p = &buf[4 + i * 4];
        /* record: Y low, Y high, X low, X high|id<<4 */
        uint16_t y = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        uint16_t x = (uint16_t)p[2] | ((uint16_t)(p[3] & 0x0F) << 8);
        tp->data.coords[i].x = x;
        tp->data.coords[i].y = y;
        tp->data.coords[i].strength = 0;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static bool gsl_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                       uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    *point_num = tp->data.points > max_point_num ? max_point_num : tp->data.points;
    for (uint8_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);
    return *point_num > 0;
}

static esp_err_t gsl_del(esp_lcd_touch_handle_t tp)
{
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    free(tp);
    return ESP_OK;
}

esp_err_t esp_lcd_touch_new_i2c_gsl3680(const esp_lcd_panel_io_handle_t io,
                                        const esp_lcd_touch_config_t *config,
                                        esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(io && config && out_touch, ESP_ERR_INVALID_ARG, TAG, "bad args");

    esp_lcd_touch_handle_t tp = heap_caps_calloc(1, sizeof(esp_lcd_touch_t), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(tp, ESP_ERR_NO_MEM, TAG, "no mem");
    tp->io = io;
    tp->read_data = gsl_read_data;
    tp->get_xy = gsl_get_xy;
    tp->del = gsl_del;
    tp->data.lock.owner = portMUX_FREE_VAL;
    memcpy(&tp->config, config, sizeof(esp_lcd_touch_config_t));

    esp_err_t ret = ESP_OK;
    if (config->rst_gpio_num != GPIO_NUM_NC) {
        gpio_config_t rst = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(config->rst_gpio_num),
        };
        ESP_GOTO_ON_ERROR(gpio_config(&rst), fail, TAG, "rst gpio");
    }
    ESP_GOTO_ON_ERROR(gsl_bringup(tp), fail, TAG, "bringup");

    if (config->int_gpio_num != GPIO_NUM_NC) {
        gpio_config_t intc = {
            .mode = GPIO_MODE_INPUT,
            .intr_type = config->levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
            .pin_bit_mask = BIT64(config->int_gpio_num),
        };
        ESP_GOTO_ON_ERROR(gpio_config(&intc), fail, TAG, "int gpio");
        if (config->interrupt_callback) {
            esp_lcd_touch_register_interrupt_callback(tp, config->interrupt_callback);
        }
    }
    *out_touch = tp;
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(ret));
    free(tp);
    return ret;
}
