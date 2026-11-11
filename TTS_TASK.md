# Task: speak chat replies through a TTS HTTP endpoint

Work in this repository (muse-gadget-sdk), branch `cores3-custom` (already
checked out). Read `esp32/AGENTS.md` first for build conventions.

## Goal

In `esp32/components/muse/muse_chat_session.cpp`, the function `start_tts()`
contains an in-code comment describing exactly how to speak replies instead of
showing them in silence: send the message text to a TTS API of your choice and
play the MP3 it returns, feeding the bytes to `tts_data()` on the session task.
Implement that recipe.

## Specification

1. Add a string Kconfig option `MUSE_TTS_ENDPOINT` in
   `esp32/components/muse/Kconfig` (near `MUSE_HATCH`): the full URL of a TTS
   HTTP endpoint. Default empty; empty keeps the current silence behaviour.
2. In `start_tts()`, when the endpoint is non-empty and a message is queued:
   - Follow the recipe comment for the turn state exactly: keep
     `m.tts = TTS_ACTIVE`, set `s_turn.tts_msg = i`, `s_turn.silent = false`,
     `m.pcm_start = s_turn.pcm_out`, `m.pcm_frames = 0`, `s_turn.mp3_len = 0`,
     `s_turn.mp3_ended = false`, `s_turn.kbps = 0`, `s_turn.down_rate = 0`,
     call `mp3dec_init(&s_turn.dec)` and `show_reply_start(m)`.
   - POST the message text as JSON `{"text": "..."}` to the endpoint using
     esp_http_client, on this same task. Build the JSON with cJSON (already
     used in this file) so UTF-8 Japanese text is escaped correctly. The text
     is `s_turn.texts + i * TEXT_MAX` when texts was allocated (up to
     TEXT_MAX - 1 bytes); if the text is unavailable for a message, fall back
     to silence for that message.
   - Stream the response body into `tts_data()` as it arrives. `tts_data()`
     drops bytes when MP3_BUF is full; follow the recipe ("hold off while
     it's full") using the same synchronisation the file already uses around
     the mp3 buffer and decode(), so audio is not dropped mid-message.
   - When the body is complete, set `s_turn.mp3_ended = true`.
   - HTTP timeout about 20 s total. On any error before audio starts, fall
     back to the existing silence pacing for that message (a turn must never
     hang). If the stream fails mid-way, still set `s_turn.mp3_ended`.
3. Set `CONFIG_MUSE_TTS_ENDPOINT="http://192.168.24.52:50022/say"` in
   `esp32/devices/sdkconfig.muse-m5stack-cores3`.
4. The endpoint returns MP3, 24 kHz mono, about 48-64 kbps, which is what the
   existing decoder path consumes. It is already running and tested; do not
   call it during development (build only).

## Build and finish

- Build with `/tmp/build_cores3.sh` (sets PATH, sources ESP-IDF v6.0.3, runs
  `tools/muse/board.sh build cores3`). Iterate until it prints BUILD_EXIT=0.
- Commit the change on `cores3-custom` with the message:
  "CoreS3: speak chat replies via TTS endpoint"
- Do NOT push. Do NOT flash.
- Report: files changed, how you handled MP3_BUF backpressure and the error
  fallback, and the build result.
