#include "muse_prox.h"

#if CONFIG_MUSE_BOARD_M5STACK_CORES3

#include <stdatomic.h>
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "muse_state.h"
#include "muse_voice.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static const char *TAG = "muse_prox";

#define SI12T_ADDR 0x68
#define REG_SENS1 0x02     /* 0x02..0x06: sensitivity */
#define REG_SENS_LAST 0x06
#define REG_CTRL1 0x08
#define REG_CTRL2 0x09
#define REG_REF_RST1 0x0A
#define REG_OUTPUT1 0x10

static i2c_master_dev_handle_t s_dev;
static atomic_int s_ch[3];
static atomic_bool s_ok;
static atomic_int s_events;
static atomic_int s_taps;
static atomic_int s_dtaps;

#define TAP_MAX_MS 400
#define DTAP_WINDOW_MS 600   /* a first tap waits this long for a second one */

#define PET_REPEAT_MS 500

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static esp_err_t wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    esp_err_t e = i2c_master_transmit(s_dev, b, 2, 50);
    if (e != ESP_OK) ESP_LOGE(TAG, "write 0x%02x=0x%02x failed (%s)", reg, v, esp_err_to_name(e));
    return e;
}

static esp_err_t si12t_init(void)
{
    esp_err_t err = ESP_OK;
    /* 1. enable channels: REF_RST1/2, CH_HOLD1/2, CAL_HOLD1/2 (0x0A..0x0F) = 0 */
    for (uint8_t r = REG_REF_RST1; r <= 0x0F; r++) {
        esp_err_t e = wr(r, 0x00);
        if (err == ESP_OK) err = e;
    }
    /* 2. CTRL2: 0x0F then 0x07 */
    esp_err_t e = wr(REG_CTRL2, 0x0F);
    if (err == ESP_OK) err = e;
    e = wr(REG_CTRL2, 0x07);
    if (err == ESP_OK) err = e;
    /* 3. CTRL1 */
    e = wr(REG_CTRL1, 0x22);
    if (err == ESP_OK) err = e;
    /* 4. sensitivity (TYPE_LOW, level 3) */
    for (uint8_t r = REG_SENS1; r <= REG_SENS_LAST; r++) {
        e = wr(r, 0x33);
        if (err == ESP_OK) err = e;
    }
    return err;
}

static void prox_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));   /* let the other I2C users start first */
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "no I2C bus: touch disabled");
        vTaskDelete(NULL);
        return;
    }
    const i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = SI12T_ADDR, .scl_speed_hz = 100000};
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02x failed (%s): touch disabled", SI12T_ADDR, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    if (si12t_init() != ESP_OK) {
        ESP_LOGW(TAG, "Si12T init had write errors (see above), trying to read anyway");
    } else {
        ESP_LOGI(TAG, "Si12T init done");
    }
    uint8_t out = 0;
    err = rd(REG_OUTPUT1, &out, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OUTPUT1 first read failed (%s): touch disabled", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    atomic_store(&s_ok, true);

    int fails = 0;
    bool touching = false;
    int64_t pet_start_us = 0, last_happy_us = 0;
    bool tap_pending = false;   /* a confirmed single tap, held back until the double-tap window is over */
    int64_t tap_due_us = 0;
    for (;;) {
        if (rd(REG_OUTPUT1, &out, 1) == ESP_OK) {
            fails = 0;
            for (int i = 0; i < 3; i++) atomic_store(&s_ch[i], (out >> (2 * i)) & 0x03);
            bool now_touch = (out & 0x3F) != 0;
            int64_t now = esp_timer_get_time();
            if (tap_pending && now >= tap_due_us) {   /* no second tap: the single tap's action goes now */
                tap_pending = false;
                int r = muse_voice_tap();
                if (r == 1) ESP_LOGI(TAG, "tap -> note start");
                else if (r == 2) ESP_LOGI(TAG, "tap -> note end");
                else ESP_LOGI(TAG, "tap ignored");
            }
            if (now_touch && !touching) {
                touching = true;
                pet_start_us = last_happy_us = now;
                atomic_fetch_add(&s_events, 1);
                ESP_LOGI(TAG, "pet start");
                muse_state_make_happy();   /* also pokes: wakes MUSE_MODE_SLEEP */
            } else if (now_touch) {
                if (now - last_happy_us >= (int64_t)PET_REPEAT_MS * 1000) {
                    last_happy_us = now;
                    muse_state_make_happy();
                }
            } else if (touching) {
                touching = false;
                int64_t held_ms = (now - pet_start_us) / 1000;
                ESP_LOGI(TAG, "pet end (%lld)", (long long)held_ms);
                if (held_ms <= TAP_MAX_MS) {
                    atomic_fetch_add(&s_taps, 1);
                    if (tap_pending) {   /* second tap inside the window: the held single tap is dropped */
                        tap_pending = false;
                        atomic_fetch_add(&s_dtaps, 1);
                        if (muse_voice_double_tap()) ESP_LOGI(TAG, "double tap -> conversation end");
                        else ESP_LOGI(TAG, "double tap ignored");
                    } else {
                        tap_pending = true;
                        tap_due_us = now + (int64_t)DTAP_WINDOW_MS * 1000;
                    }
                }
            }
        } else if (++fails >= 20) {
            ESP_LOGE(TAG, "OUTPUT1 read keeps failing: touch disabled");
            atomic_store(&s_ok, false);
            for (int i = 0; i < 3; i++) atomic_store(&s_ch[i], 0);
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void muse_prox_start(void)
{
    TaskHandle_t t;
    if (xTaskCreateWithCaps(prox_task, "prox", 3072, NULL, 3, &t, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "prox task not started: touch disabled");
    }
}

bool muse_prox_available(void) { return atomic_load(&s_ok); }
int muse_prox_taps(void) { return atomic_load(&s_taps); }
int muse_prox_dtaps(void) { return atomic_load(&s_dtaps); }
int muse_prox_events(void) { return atomic_load(&s_events); }
int muse_prox_touch(int ch) { return (ch >= 0 && ch < 3 && atomic_load(&s_ok)) ? atomic_load(&s_ch[ch]) : 0; }

#endif
