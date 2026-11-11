/*
 * Local extension of this fork: StackChan neck servos on the M5Stack CoreS3 body.
 * See muse_servo.h for the wiring. The SCSCL protocol (PING, READ, WRITE) is
 * implemented here from the public packet format; no vendor library is used.
 *
 * Packet (both directions): FF FF ID LEN INST/ERR PARAMS... CHK, LEN = params + 2,
 * CHK = ~(ID + LEN + INST + PARAMS) & 0xFF. 16-bit values are big-endian.
 * Registers used: 0x2A goal position (then time, speed), 0x38 present position.
 *
 * The body's IO expander (PY32, M5IOE1 family) switches the servo supply (VM EN,
 * pin 0). Its register map (16-bit pairs: MODE 0x03, OUT 0x05, OD 0x13) is the one
 * M5's other M5IOE1 boards use (board_m5stack_stopwatch.c); it is NOT confirmed
 * for the 0x6F part by anything in this tree, so start-up reads the output
 * register back and logs it.
 */
#include "muse_servo.h"

#if CONFIG_MUSE_BOARD_M5STACK_CORES3

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static const char *TAG = "servo";

/* ---- Wiring and calibration (M5Stack StackChan K151 public BSP data) ---- */
#define SERVO_UART UART_NUM_1
#define SERVO_BAUD 1000000
#define SERVO_TX 6
#define SERVO_RX 7
#define ID_YAW 1
#define ID_PITCH 2
#define RAW_MAX 1000
#define RAW_PER_DEG (1000.0f / 300.0f)   /* the SCS0009 turns 300 degrees over 0..1000 */
#define YAW_ZERO_RAW 460
#define PITCH_ZERO_RAW 620
#define SEED_YAW 0.0f
#define SEED_PITCH 20.0f

#define IOE_ADDR 0x6F
#define IOE_MODE 0x03   /* 16 bits, IO1 in bit 0; 1 = output */
#define IOE_OUT 0x05
#define IOE_OD 0x13     /* 1 = open drain */
#define VM_EN_BIT 0x0001   /* pin 0 */
#define VM_EN_SETTLE_MS 250
#define POWER_TRIES 10     /* x 50 ms after the settle */

#define MAX_DPS 90.0f          /* speed limit of a commanded move */
#define BOOT_DPS 25.0f         /* the first move to neutral */
#define STEP_MS 20
#define MAX_MOVE_MS 12000
#define LISTEN_LIFT_DEG 8.0f
#define LISTEN_HOLD_US (10 * 1000000LL)

/* SCSCL instructions and registers */
#define INST_PING 0x01
#define INST_READ 0x02
#define INST_WRITE 0x03
#define REG_GOAL_POS 0x2A
#define REG_PRESENT_POS 0x38

/* ---- State ---- */
typedef enum { JOB_NONE, JOB_MOVE, JOB_GESTURE, JOB_LISTEN } job_kind_t;
enum { J_IDLE, J_SETUP, J_PENDING, J_RUNNING };

static struct {
    job_kind_t kind;
    float yaw, pitch;
    int ms;
    muse_gesture_t gesture;
    muse_servo_cb cb;
    void *ctx;
} s_job;
static atomic_int s_job_state = J_IDLE;
static TaskHandle_t s_task;
static i2c_master_dev_handle_t s_ioe;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_servo_state_t s_state;   /* guarded by s_lock */
static int64_t s_return_at_us;       /* worker only: when the listen pose ends */
static float s_return_pitch;

/* ---- Angles ---- */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int yaw_to_raw(float d) { return (int)lroundf(clampf(YAW_ZERO_RAW + d * RAW_PER_DEG, 0, RAW_MAX)); }
static int pitch_to_raw(float d) { return (int)lroundf(clampf(PITCH_ZERO_RAW + d * RAW_PER_DEG, 0, RAW_MAX)); }
static float raw_to_yaw(int r) { return (r - YAW_ZERO_RAW) / RAW_PER_DEG; }
static float raw_to_pitch(int r) { return (r - PITCH_ZERO_RAW) / RAW_PER_DEG; }

/* ---- SCSCL ---- */
static size_t build(uint8_t *p, uint8_t id, uint8_t inst, const uint8_t *params, size_t n)
{
    p[0] = p[1] = 0xFF;
    p[2] = id;
    p[3] = (uint8_t)(n + 2);
    p[4] = inst;
    memcpy(p + 5, params, n);
    uint8_t sum = id + p[3] + inst;
    for (size_t i = 0; i < n; i++) sum += params[i];
    p[5 + n] = (uint8_t)~sum;
    return n + 6;
}

/* Finds the status packet from `id` with `nparams` data bytes; skips an echo of our own request. */
static bool find_status(const uint8_t *b, size_t n, uint8_t id, size_t nparams, const uint8_t *req, size_t reqn,
                        uint8_t *err, uint8_t *data)
{
    size_t total = nparams + 6;
    for (size_t i = 0; i + total <= n; i++) {
        if (b[i] != 0xFF || b[i + 1] != 0xFF || b[i + 2] != id || b[i + 3] != nparams + 2) continue;
        if (reqn == total && memcmp(b + i, req, total) == 0) continue;   /* our own echo */
        uint8_t sum = 0;
        for (size_t k = 2; k < total - 1; k++) sum += b[i + k];
        if ((uint8_t)~sum != b[i + total - 1]) continue;
        *err = b[i + 4];
        memcpy(data, b + i + 5, nparams);
        return true;
    }
    return false;
}

static bool transact(uint8_t id, uint8_t inst, const uint8_t *params, size_t n, size_t resp_params, uint8_t *data)
{
    uint8_t req[16], rx[32];
    size_t reqn = build(req, id, inst, params, n);
    uart_flush_input(SERVO_UART);
    uart_write_bytes(SERVO_UART, req, reqn);
    if (inst == INST_WRITE) {
        return true;   /* the status reply is not waited for; the next transaction flushes it */
    }
    uart_wait_tx_done(SERVO_UART, pdMS_TO_TICKS(10));
    size_t got = 0;
    for (int tries = 0; tries < 2 && got < sizeof(rx); tries++) {
        int r = uart_read_bytes(SERVO_UART, rx + got, sizeof(rx) - got, pdMS_TO_TICKS(15));
        if (r > 0) got += r;
        uint8_t err;
        if (find_status(rx, got, id, resp_params, req, reqn, &err, data)) {
            if (err) ESP_LOGW(TAG, "servo %u status error bits 0x%02x", id, err);
            return true;
        }
    }
    return false;
}

static bool servo_ping(uint8_t id)
{
    uint8_t d[1];
    return transact(id, INST_PING, NULL, 0, 0, d);
}

static bool servo_read_pos(uint8_t id, int *raw)
{
    const uint8_t p[2] = { REG_PRESENT_POS, 2 };
    uint8_t d[2];
    if (!transact(id, INST_READ, p, 2, 2, d)) return false;
    *raw = (d[0] << 8) | d[1];
    return true;
}

static void servo_write_pos(uint8_t id, int raw, int time_ms)
{
    const uint8_t p[7] = { REG_GOAL_POS, raw >> 8, raw & 0xFF, time_ms >> 8, time_ms & 0xFF, 0, 0 };
    transact(id, INST_WRITE, p, sizeof(p), 0, NULL);
}

/* ---- IO expander ---- */
static esp_err_t ioe_update(uint8_t reg, uint16_t mask, uint16_t bits)
{
    uint8_t b[2];
    esp_err_t err = i2c_master_transmit_receive(s_ioe, &reg, 1, b, 2, 100);
    if (err != ESP_OK) return err;
    uint16_t v = (uint16_t)(b[0] | b[1] << 8);
    v = (uint16_t)((v & ~mask) | (bits & mask));
    uint8_t w[3] = { reg, (uint8_t)v, (uint8_t)(v >> 8) };
    return i2c_master_transmit(s_ioe, w, sizeof(w), 100);
}

static esp_err_t ioe_read(uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    esp_err_t err = i2c_master_transmit_receive(s_ioe, &reg, 1, b, 2, 100);
    if (err == ESP_OK) *v = (uint16_t)(b[0] | b[1] << 8);
    return err;
}

static esp_err_t vm_en_set(bool on)
{
    esp_err_t err = ioe_update(IOE_OD, VM_EN_BIT, 0);   /* push-pull */
    if (err == ESP_OK) err = ioe_update(IOE_OUT, VM_EN_BIT, on ? VM_EN_BIT : 0);
    if (err == ESP_OK) err = ioe_update(IOE_MODE, VM_EN_BIT, VM_EN_BIT);   /* output */
    uint16_t out = 0, mode = 0;
    if (err == ESP_OK) err = ioe_read(IOE_OUT, &out);
    if (err == ESP_OK) err = ioe_read(IOE_MODE, &mode);
    if (err == ESP_OK) {
        bool level = out & VM_EN_BIT;
        ESP_LOGI(TAG, "VM EN %s: OUT=0x%04x MODE=0x%04x (pin 0 reads %s)", on ? "on" : "off", out, mode,
                 level ? "high" : "low");
        if (level != on || !(mode & VM_EN_BIT)) err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "VM EN %s failed: %s", on ? "on" : "off", esp_err_to_name(err));
    portENTER_CRITICAL(&s_lock);
    if (err == ESP_OK) s_state.vm_en = on;
    portEXIT_CRITICAL(&s_lock);
    return err;
}

/* ---- Position ---- */
static void set_pos(float yaw, float pitch, bool valid, bool seeded)
{
    portENTER_CRITICAL(&s_lock);
    s_state.yaw = yaw;
    s_state.pitch = pitch;
    s_state.pos_valid = valid;
    s_state.seeded = seeded;
    portEXIT_CRITICAL(&s_lock);
}

/* Reads both servos, retrying: the supply needs ~200 ms after VM EN before they answer. */
static bool read_both(int tries, float *yaw, float *pitch)
{
    for (int i = 0; i < tries; i++) {
        int ry, rp;
        bool oky = servo_read_pos(ID_YAW, &ry);
        bool okp = servo_read_pos(ID_PITCH, &rp);
        if (oky && okp) {
            *yaw = raw_to_yaw(ry);
            *pitch = raw_to_pitch(rp);
            ESP_LOGI(TAG, "position read (try %d): yaw raw %d = %.1f deg, pitch raw %d = %.1f deg", i + 1, ry, *yaw, rp,
                     *pitch);
            return true;
        }
        ESP_LOGW(TAG, "position read failed (try %d/%d): yaw %s, pitch %s", i + 1, tries, oky ? "ok" : "no answer",
                 okp ? "ok" : "no answer");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

/* ---- Motion ---- */
static float s_cur_yaw, s_cur_pitch;   /* worker only */

static void drive(float yaw, float pitch)
{
    servo_write_pos(ID_YAW, yaw_to_raw(yaw), STEP_MS * 2);
    servo_write_pos(ID_PITCH, pitch_to_raw(pitch), STEP_MS * 2);
}

/* Smooth move from the current angles; the duration is at least what `dps` allows. */
static void move_to(float yaw, float pitch, int ms, float dps)
{
    yaw = clampf(yaw, -MUSE_SERVO_YAW_MAX, MUSE_SERVO_YAW_MAX);
    pitch = clampf(pitch, MUSE_SERVO_PITCH_MIN, MUSE_SERVO_PITCH_MAX);
    float dy = yaw - s_cur_yaw, dp = pitch - s_cur_pitch;
    float far = fmaxf(fabsf(dy), fabsf(dp));
    int need = (int)(far / dps * 1000.0f);
    int dur = ms > need ? ms : need;
    if (dur > MAX_MOVE_MS) dur = MAX_MOVE_MS;
    ESP_LOGI(TAG, "move start: yaw %.1f -> %.1f, pitch %.1f -> %.1f over %d ms", s_cur_yaw, yaw, s_cur_pitch, pitch, dur);
    portENTER_CRITICAL(&s_lock);
    s_state.moving = true;
    portEXIT_CRITICAL(&s_lock);
    float y0 = s_cur_yaw, p0 = s_cur_pitch;
    int64_t t0 = esp_timer_get_time();
    while (true) {
        int64_t el = (esp_timer_get_time() - t0) / 1000;
        if (el >= dur) break;
        float t = (float)el / dur;
        float e = t * t * (3.0f - 2.0f * t);   /* smoothstep */
        drive(y0 + (yaw - y0) * e, p0 + (pitch - p0) * e);
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
    drive(yaw, pitch);
    s_cur_yaw = yaw;
    s_cur_pitch = pitch;
    set_pos(yaw, pitch, true, false);
    portENTER_CRITICAL(&s_lock);
    s_state.moving = false;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "move done: yaw %.1f, pitch %.1f", yaw, pitch);
}

/* Before a job: the position must be real, never a guess (a guessed start can sweep through a hard stop). */
static bool ensure_position(void)
{
    muse_servo_state_t st;
    muse_servo_get_state(&st);
    if (st.pos_valid) return true;
    float y, p;
    if (!read_both(5, &y, &p)) return false;
    s_cur_yaw = y;
    s_cur_pitch = p;
    set_pos(y, p, true, false);
    return true;
}

static void run_gesture(muse_gesture_t g)
{
    float y = s_cur_yaw, p = s_cur_pitch;
    switch (g) {
    case MUSE_GESTURE_NOD:
        for (int i = 0; i < 2; i++) {
            move_to(y, p - 12.0f, 250, MAX_DPS);
            move_to(y, p, 250, MAX_DPS);
        }
        break;
    case MUSE_GESTURE_SHAKE:
        move_to(y + 15.0f, p, 200, MAX_DPS);
        for (int i = 0; i < 2; i++) {
            move_to(y - 15.0f, p, 350, MAX_DPS);
            move_to(y + 15.0f, p, 350, MAX_DPS);
        }
        move_to(y, p, 200, MAX_DPS);
        break;
    case MUSE_GESTURE_LOOK_UP:
        move_to(y, p + 15.0f, 500, MAX_DPS);
        break;
    case MUSE_GESTURE_CENTER:
        move_to(MUSE_SERVO_NEUTRAL_YAW, MUSE_SERVO_NEUTRAL_PITCH, 600, MAX_DPS);
        break;
    }
}

static void finish(bool ok, const char *code, const char *message)
{
    muse_servo_cb cb = s_job.cb;
    void *ctx = s_job.ctx;
    memset(&s_job, 0, sizeof(s_job));
    atomic_store(&s_job_state, J_IDLE);
    if (cb) cb(ctx, ok, code, message);
}

static void run_job(void)
{
    job_kind_t kind = s_job.kind;
    if (kind != JOB_LISTEN) s_return_at_us = 0;
    if (!ensure_position()) {
        ESP_LOGW(TAG, "job refused: servos do not answer a position read");
        finish(false, "servo_unreachable", "servos did not answer a position read");
        return;
    }
    switch (kind) {
    case JOB_MOVE:
        move_to(s_job.yaw, s_job.pitch, s_job.ms, MAX_DPS);
        break;
    case JOB_GESTURE:
        ESP_LOGI(TAG, "gesture %d", (int)s_job.gesture);
        run_gesture(s_job.gesture);
        break;
    case JOB_LISTEN:
        if (s_return_at_us == 0) s_return_pitch = s_cur_pitch;
        move_to(s_cur_yaw, s_return_pitch + LISTEN_LIFT_DEG, 300, MAX_DPS);
        s_return_at_us = esp_timer_get_time() + LISTEN_HOLD_US;
        break;
    default:
        break;
    }
    finish(true, NULL, NULL);
}

static void servo_task(void *arg)
{
    (void)arg;
    /* Bus first, so the lines are idle-high before the servos get power. */
    uart_config_t cfg = {
        .baud_rate = SERVO_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(SERVO_UART, 256, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(SERVO_UART, &cfg);
    if (err == ESP_OK) err = uart_set_pin(SERVO_UART, SERVO_TX, SERVO_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART1 setup failed (%s): servos disabled", esp_err_to_name(err));
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "UART1 %d baud, TX GPIO%d RX GPIO%d", SERVO_BAUD, SERVO_TX, SERVO_RX);

    if (vm_en_set(true) != ESP_OK) {
        ESP_LOGE(TAG, "servo supply not switched on: servos disabled");
        vTaskDelete(NULL);
    }
    vTaskDelay(pdMS_TO_TICKS(VM_EN_SETTLE_MS));
    bool pinged = false;
    for (int i = 0; i < POWER_TRIES && !pinged; i++) {
        pinged = servo_ping(ID_YAW) && servo_ping(ID_PITCH);
        if (!pinged) vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "servo ping (yaw id %d, pitch id %d): %s", ID_YAW, ID_PITCH, pinged ? "both answered" : "no answer");

    portENTER_CRITICAL(&s_lock);
    s_state.available = true;
    portEXIT_CRITICAL(&s_lock);

    float y, p;
    if (read_both(POWER_TRIES, &y, &p)) {
        s_cur_yaw = y;
        s_cur_pitch = p;
        set_pos(y, p, true, false);
        move_to(MUSE_SERVO_NEUTRAL_YAW, MUSE_SERVO_NEUTRAL_PITCH, 2000, BOOT_DPS);
    } else {
        /* Report the seed, but do not move on a guess; the next job retries the read. */
        s_cur_yaw = SEED_YAW;
        s_cur_pitch = SEED_PITCH;
        set_pos(SEED_YAW, SEED_PITCH, false, true);
        ESP_LOGE(TAG, "position read failed to the end: not moving (state shows seed yaw %.0f pitch %.0f)", SEED_YAW,
                 SEED_PITCH);
    }

    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (s_return_at_us) {
            int64_t left = s_return_at_us - esp_timer_get_time();
            wait = left > 0 ? pdMS_TO_TICKS(left / 1000 + 1) : 0;
        }
        ulTaskNotifyTake(pdTRUE, wait);
        if (atomic_load(&s_job_state) == J_PENDING) {
            atomic_store(&s_job_state, J_RUNNING);
            run_job();
        } else if (s_return_at_us && esp_timer_get_time() >= s_return_at_us) {
            s_return_at_us = 0;
            int idle = J_IDLE;
            if (atomic_compare_exchange_strong(&s_job_state, &idle, J_RUNNING)) {
                ESP_LOGI(TAG, "listen pose over: pitch back to %.1f", s_return_pitch);
                muse_servo_state_t st;
                muse_servo_get_state(&st);
                if (st.pos_valid) move_to(s_cur_yaw, s_return_pitch, 600, MAX_DPS);
                atomic_store(&s_job_state, J_IDLE);
            }
        }
    }
}

/* ---- API ---- */
void muse_servo_start(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "no I2C bus: servos disabled");
        return;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = i2c_master_probe(bus, IOE_ADDR, 50);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IO expander not found at 0x%02x (%s): servos disabled. I2C devices present:", IOE_ADDR,
                 esp_err_to_name(err));
        for (uint16_t a = 0x08; a < 0x78; a++) {
            if (i2c_master_probe(bus, a, 20) == ESP_OK) ESP_LOGI(TAG, "  0x%02x", a);
        }
        return;
    }
    ESP_LOGI(TAG, "IO expander found at 0x%02x", IOE_ADDR);
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IOE_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &dev, &s_ioe);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "expander add failed (%s): servos disabled", esp_err_to_name(err));
        return;
    }
    if (xTaskCreateWithCaps(servo_task, "servo", 4096, NULL, 4, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "servo task not started: servos disabled");
    }
}

void muse_servo_get_state(muse_servo_state_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_state;
    portEXIT_CRITICAL(&s_lock);
}

static bool submit(job_kind_t kind, float yaw, float pitch, int ms, muse_gesture_t g, muse_servo_cb cb, void *ctx,
                   const char **code, const char **message)
{
    muse_servo_state_t st;
    muse_servo_get_state(&st);
    if (!st.available || !s_task) {
        *code = "unavailable";
        *message = "head servos are not available";
        return false;
    }
    int idle = J_IDLE;
    if (!atomic_compare_exchange_strong(&s_job_state, &idle, J_SETUP)) {
        *code = "busy";
        *message = "the head is already moving";
        return false;
    }
    s_job.kind = kind;
    s_job.yaw = yaw;
    s_job.pitch = pitch;
    s_job.ms = ms;
    s_job.gesture = g;
    s_job.cb = cb;
    s_job.ctx = ctx;
    atomic_store(&s_job_state, J_PENDING);
    xTaskNotifyGive(s_task);
    return true;
}

bool muse_servo_move(float yaw, float pitch, int ms, bool *clamped, muse_servo_cb cb, void *ctx, const char **code,
                     const char **message)
{
    float cy = clampf(yaw, -MUSE_SERVO_YAW_MAX, MUSE_SERVO_YAW_MAX);
    float cp = clampf(pitch, MUSE_SERVO_PITCH_MIN, MUSE_SERVO_PITCH_MAX);
    *clamped = cy != yaw || cp != pitch;
    if (*clamped) {
        ESP_LOGW(TAG, "clamped: requested yaw %.1f pitch %.1f -> yaw %.1f pitch %.1f", yaw, pitch, cy, cp);
    }
    if (ms < 0) ms = 0;
    bool ok = submit(JOB_MOVE, cy, cp, ms, MUSE_GESTURE_CENTER, cb, ctx, code, message);
    if (!ok) ESP_LOGW(TAG, "head.move refused: %s", *code);
    return ok;
}

bool muse_servo_gesture(muse_gesture_t g, muse_servo_cb cb, void *ctx, const char **code, const char **message)
{
    bool ok = submit(JOB_GESTURE, 0, 0, 0, g, cb, ctx, code, message);
    if (!ok) ESP_LOGW(TAG, "head.gesture refused: %s", *code);
    return ok;
}

void muse_servo_listen_pose(void)
{
    const char *code, *message;
    if (submit(JOB_LISTEN, 0, 0, 0, MUSE_GESTURE_CENTER, NULL, NULL, &code, &message)) {
        ESP_LOGI(TAG, "wake: lifting the head a little");
    }
}

#endif /* CONFIG_MUSE_BOARD_M5STACK_CORES3 */
