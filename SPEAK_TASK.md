# Implementation task: add a "speak" device command (proactive TTS)

Repo: ~/dev/muse-gadget-sdk (branch: cores3-custom). Work in esp32/.

## Goal

Today the gadget speaks only as part of a chat turn, via the TTS endpoint
(commit cf08bd5: CONFIG_MUSE_TTS_ENDPOINT, tts_fetch() in
components/muse/muse_chat_session.cpp). The user wants Muse (the agent) to
be able to make the gadget speak arbitrary text WITHOUT a chat turn — for
greetings and notifications ("こちらから話しかける").

Add a new DEVICE COMMAND that the platform can invoke on the gadget —
following the same pattern as the existing commands (device.health,
display.draw_url, ...) — that speaks a given text through the same TTS
endpoint and the speaker.

## Before writing code, read

- main/noise_control.cpp — how commands are defined (add_command),
  registered (ble_commands_register), dispatched and replied to. Mirror
  display.draw_url end to end (schema, handler, completion reply).
- components/muse/muse_chat_session.cpp — the cf08bd5 TTS implementation
  (tts_fetch, the decode/caption flow, backpressure handling).

## Requirements

1. Command definition:
   - Name: `speak` (if the existing naming pattern suggests a better fit,
     use it and note it in the report).
   - One required parameter: `text` (string, UTF-8, at most ~400 chars).
   - The command must appear in the device's advertised command list (the
     same list the existing commands are in), with a short description.
2. Behaviour:
   - POST {"text": ...} to CONFIG_MUSE_TTS_ENDPOINT and play the returned
     MP3 on the speaker at the device's current volume, REUSING the
     existing fetch/decode machinery from cf08bd5 as far as it cleanly
     goes. Expose a small callable entry point from the muse component
     (e.g. in a header) so main/noise_control.cpp can invoke it.
   - The command's reply is sent when playback has FINISHED (success), so
     the caller knows the utterance completed.
   - Show the spoken text on screen as a caption while speaking, IF that
     can be done cleanly through the existing caption path; if it cannot,
     say so in the report instead of forcing it.
   - Concurrency: if a chat turn is active (or another speak is playing),
     do NOT corrupt it — return an error reply (e.g. "busy") so the caller
     can retry later. Never hang: overall cap ~30s, after which the
     command replies with an error.
   - If CONFIG_MUSE_TTS_ENDPOINT is empty, the command replies with an
     error ("tts not configured") and does nothing else.
   - On any HTTP failure, reply with an error; the gadget must stay fully
     usable afterwards.
3. The firmware header comment for the command should note it is a local
   extension of this fork (like the TTS change does).
4. No changes to the chat-turn behaviour: existing push-to-talk replies
   must speak exactly as before.

## Build & commit

- Build with /tmp/build_cores3.sh until BUILD_EXIT=0, no new warnings in
  the files you touch.
- Commit on cores3-custom with message:
  "Add speak command: say arbitrary text via the TTS endpoint"
- Do NOT push. Do NOT flash (speak_no_flash).

## Report back

- The command name chosen and its final parameter schema.
- Where the entry point lives and how playback completion is signalled.
- Whether captions were included, and if not, why.
- How the busy case is detected.
- Files changed + diff stat, build result, anything left unverified.
