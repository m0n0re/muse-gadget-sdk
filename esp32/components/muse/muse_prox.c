#include "muse_prox.h"

#if CONFIG_MUSE_BOARD_M5STACK_CORES3

#include <stdatomic.h>
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static const char *TAG = "muse_prox";

#define LTR_ADDR 0x23
#define REG_ALS_CONTR 0x80
#define REG_PS_CONTR 0x81
#define REG_PS_LED 0x82
#define REG_PS_N_PULSES 0x83
#define REG_PS_MEAS_RATE 0x84
#define REG_PART_ID 0x86
#define REG_PS_DATA 0x8D   /* PS_DATA_0 (low 8), PS_DATA_1 (bit7 saturation, bits2:0 high) */

static i2c_master_dev_handle_t s_dev;
static atomic_int s_ps = -1;
static atomic_bool s_sat;
static atomic_bool s_ok;
static atomic_int s_part = -1;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static esp_err_t wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(s_dev, b, 2, 50);
}

static void prox_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));   /* let the other I2C users start first */
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "no I2C bus: proximity disabled");
        vTaskDelete(NULL);
        return;
    }
    const i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = LTR_ADDR, .scl_speed_hz = 100000};
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02x failed (%s): proximity disabled", LTR_ADDR, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    uint8_t id = 0;
    err = rd(REG_PART_ID, &id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PART_ID read failed (%s): proximity disabled", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    atomic_store(&s_part, id);
    ESP_LOGI(TAG, "PART_ID 0x%02x (%s, expected 0x92)", id, id == 0x92 ? "match" : "MISMATCH");

    /* PS_LED 0x7C: 60 kHz, 100% duty, 100 mA. PULSES 8. MEAS_RATE 50 ms. PS_CONTR 0x23: active + saturation indicator. */
    err = wr(REG_PS_LED, 0x7C);
    if (err == ESP_OK) err = wr(REG_PS_N_PULSES, 0x08);
    if (err == ESP_OK) err = wr(REG_PS_MEAS_RATE, 0x00);
    if (err == ESP_OK) err = wr(REG_PS_CONTR, 0x23);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PS setup failed (%s): proximity disabled", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    (void)REG_ALS_CONTR;
    atomic_store(&s_ok, true);

    int fails = 0;
    for (;;) {
        uint8_t d[2];
        if (rd(REG_PS_DATA, d, 2) == ESP_OK) {
            fails = 0;
            int ps = d[0] | ((d[1] & 0x07) << 8);
            bool sat = d[1] & 0x80;
            atomic_store(&s_ps, ps);
            atomic_store(&s_sat, sat);
            /* TEMPORARY diagnostic log (100 ms): remove in the next stage (tap/pat detection). */
            ESP_LOGI(TAG, "ps=%d sat=%d", ps, sat);
        } else if (++fails >= 20) {
            ESP_LOGE(TAG, "PS read keeps failing: proximity disabled");
            atomic_store(&s_ok, false);
            atomic_store(&s_ps, -1);
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void muse_prox_start(void)
{
    TaskHandle_t t;
    if (xTaskCreateWithCaps(prox_task, "prox", 3072, NULL, 3, &t, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "prox task not started: proximity disabled");
    }
}

bool muse_prox_available(void) { return atomic_load(&s_ok); }
int muse_prox_ps(void) { return atomic_load(&s_ok) ? atomic_load(&s_ps) : -1; }
bool muse_prox_saturated(void) { return atomic_load(&s_sat); }
int muse_prox_part_id(void) { return atomic_load(&s_part); }

#endif
