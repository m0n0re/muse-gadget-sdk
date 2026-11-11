# Revision task: wake word must start the voice-NOTE path, not the call

Repo: ~/dev/muse-gadget-sdk (branch: cores3-custom). Work in esp32/.
Previous commit: 9904cd1 "CoreS3: wake word 'Hi Muse' starts a voice
turn" — keep its listener/cooldown/ownership work; this task changes
WHAT a detection starts.

## What the user verified on the real device

- **Tap** the button (press and release, then speak): starts the live
  CALL path (`run_call`) → the service answers **404** ("muse didnt
  take the call"). Broken in practice.
- **Hold** the button while speaking: the voice-NOTE path → the note is
  sent as a message, Muse replies, the reply is spoken. **This is the
  path that works.**

So a wake detection must behave like the button being HELD, not tapped.

## Change

1. On wake detection, start the voice-note recording through the same
   code path a held button uses (find how `voice_task` / the note
   machinery starts a note when the button goes down — reuse it; do
   not reimplement recording).
2. A wake-started note has no button to release, so end it on
   **silence**:
   - Once speech has been captured, end the note after roughly
     1.0-1.5 s of continuous silence, then send it exactly as a
     button note is sent.
   - If no speech is detected within about 4 s of the wake, cancel the
     note quietly (send nothing, return to idle listening).
   - Hard cap the note at 30 s.
   - Use whatever voice-activity signal the existing stack already has
     (the note path's own metering, or ESP-SR's VAD from the AFE you
     already added) — pick the one that fits the code best and say
     which you used.
3. Everything else from 9904cd1 stays: idle-only feeding, microphone
   ownership handoff, no self-trigger while anything plays, the
   2.5 s cooldown, the Kconfig switch, the model partition.
4. The talk button itself must behave exactly as before (both tap and
   hold).

## Build & commit

- Build with /tmp/build_cores3.sh until BUILD_EXIT=0, no new warnings
  in files you touch.
- Commit on cores3-custom with the message:
  "Wake word: start a voice note (hold path), end on silence"
- Do NOT push. Do NOT flash (no_flash).

## Report back

- Which note-path entry you reused and where the "virtual hold" begins
  and ends.
- The silence/end conditions as implemented (thresholds, which VAD or
  metering signal).
- What happens to a wake-started note if the user presses the button
  mid-recording (who wins).
- Build result and binary size.
