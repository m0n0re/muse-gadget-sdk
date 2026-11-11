/*
 * Local extension of this fork: the StackChan neck (two Feetech SCS0009 servos,
 * SCSCL protocol) on the M5Stack CoreS3 body (CONFIG_MUSE_BOARD_M5STACK_CORES3).
 * Other boards get empty stubs.
 *
 *   UART1, 1 Mbaud, TX = GPIO6, RX = GPIO7 (half-duplex bus). ID 1 = yaw,
 *   ID 2 = pitch. Servo power (VM EN) is pin 0 of the body's IO expander
 *   (PY32 / M5IOE1) on the BSP's I2C bus (SCL 11, SDA 12).
 *
 * Angles are degrees. Yaw 0 = ahead, pitch 0 = the servo's zero position
 * (pitch is only ever commanded within 5..85). Everything is clamped here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_SERVO_YAW_MAX 120.0f
#define MUSE_SERVO_PITCH_MIN 5.0f
#define MUSE_SERVO_PITCH_MAX 85.0f
#define MUSE_SERVO_NEUTRAL_YAW 0.0f
#define MUSE_SERVO_NEUTRAL_PITCH 45.0f

typedef enum {
    MUSE_GESTURE_NOD,
    MUSE_GESTURE_SHAKE,
    MUSE_GESTURE_LOOK_UP,
    MUSE_GESTURE_CENTER,
} muse_gesture_t;

typedef struct {
    bool available;   /* expander found, servo task running */
    bool vm_en;       /* the last VM EN level we drove */
    bool pos_valid;   /* the angles below come from a real read or a finished move */
    bool seeded;      /* ... or from the seed values after the read failed */
    bool moving;
    float yaw, pitch;
} muse_servo_state_t;

/* Called by the job when it ends: ok, then the error code ("clamped" never fails). */
typedef void (*muse_servo_cb)(void *ctx, bool ok, const char *code, const char *message);

#if CONFIG_MUSE_BOARD_M5STACK_CORES3
/* After the board init (the I2C bus is up). Returns at once; the power-up and the move to
 * neutral run on the servo task. Logs and stays disabled if the expander is not found. */
void muse_servo_start(void);
void muse_servo_get_state(muse_servo_state_t *out);
/* Moves to (yaw, pitch), clamped (*clamped says so), over about `ms` (0: speed limit only).
 * False with code and message set if it can't be taken ("unavailable", "busy"). Otherwise `cb`
 * runs exactly once, on the servo task, when the move has finished. */
bool muse_servo_move(float yaw, float pitch, int ms, bool *clamped, muse_servo_cb cb, void *ctx,
                     const char **code, const char **message);
bool muse_servo_gesture(muse_gesture_t g, muse_servo_cb cb, void *ctx, const char **code, const char **message);
/* The wake word started a voice note: lift the head a little, and return it a few seconds
 * later. Fire and forget; does nothing if the servos are busy or unavailable. */
void muse_servo_listen_pose(void);
#else
static inline void muse_servo_start(void) {}
static inline void muse_servo_get_state(muse_servo_state_t *out) { *out = (muse_servo_state_t){0}; }
static inline void muse_servo_listen_pose(void) {}
#endif

#ifdef __cplusplus
}
#endif
