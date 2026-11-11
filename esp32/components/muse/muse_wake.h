/*
 * Local extension of this fork: the wake word "Hi Muse" (ESP-SR WakeNet10,
 * model wn10_himuse), CONFIG_MUSE_WAKE_WORD_HI_MUSE only.
 *
 * Microphone ownership: the voice task is the only reader of the microphone.
 * While idle it passes each 20 ms chunk it already read to muse_wake_feed();
 * a detector task, which never touches the audio input, pulls them from a
 * ring buffer. Whenever the voice task leaves idle (a call, a note, a reply,
 * speak, a bench test, rest) it stops feeding; muse_wake_feed() notices the
 * gap, drops stale audio and holds the detector off for a cooldown.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MUSE_WAKE_WORD_HI_MUSE
/* Loads the model and starts the detector task. False: no wake word (logged). */
bool muse_wake_start(void);
/* The link's "up" check. With one set, muse_wake_start_deferred() waits for it. */
void muse_wake_set_ready_check(bool (*ready)(void));
/* Starts the detector later, from its own task, once the ready check passes
 * (or after a timeout, so it still starts offline). Returns at once. */
void muse_wake_start_deferred(void);
/* Initialises esp-dl's C++ singletons early, from a task context, exactly once. Idempotent. */
void muse_wake_warmup(void);
/* Idle mic audio, mono 16 kHz, from the voice task. Never blocks. */
void muse_wake_feed(const int16_t *pcm, size_t frames);
/* True once per detection since the last call. */
bool muse_wake_take(void);
#else
static inline bool muse_wake_start(void) { return false; }
static inline void muse_wake_set_ready_check(bool (*ready)(void)) { (void)ready; }
static inline void muse_wake_start_deferred(void) {}
static inline void muse_wake_warmup(void) {}
static inline void muse_wake_feed(const int16_t *pcm, size_t frames) { (void)pcm; (void)frames; }
static inline bool muse_wake_take(void) { return false; }
#endif

#ifdef __cplusplus
}
#endif
