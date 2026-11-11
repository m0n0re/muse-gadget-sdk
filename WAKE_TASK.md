# Implementation task: wake word "Hi Muse" for the CoreS3 gadget

Repo: ~/dev/muse-gadget-sdk (branch: cores3-custom). Work in esp32/.

## Goal

Today the gadget starts a voice turn only from the push-to-talk button.
Add hands-free activation: while the gadget is idle, listen for the wake
word **"Hi Muse"** (ESP-SR WakeNet10 model `wn10_himuse`) and, on
detection, start a voice turn exactly as if the talk button had been
pressed.

## Before writing code, read

- How voice turns start today: the talk-button path from the board /
  UI layer down into components/muse/muse_chat_session.cpp — find the
  single entry point a button press ultimately calls, and reuse it.
- How microphone audio is captured for a turn (which component owns the
  audio input, sample rate/format, and the task that pumps it).
- ESP-SR (espressif/esp-sr) as an ESP-IDF component: how WakeNet models
  are selected (Kconfig), what audio format the AFE/WakeNet feed
  expects, and how models are stored (flash/PSRAM).

## Requirements

1. Add ESP-SR to the build (IDF component registry) with ONLY the
   WakeNet10 "Hi Muse" model enabled. No other SR features (no command
   recognition, no TTS) in this task.
2. While the gadget is IDLE (no chat turn, no call, no OTA, not
   speaking), feed microphone audio to WakeNet continuously. On
   detection:
   - Stop/pause the wake feed and start a voice turn through the SAME
     entry point the talk button uses (the listening UI, end-of-turn
     detection and everything else must behave exactly like a
     button-started turn).
   - When the turn ends and the gadget is idle again, resume the wake
     feed after a short cooldown (2-3 seconds) so the tail of the
     conversation does not retrigger it.
3. The gadget's own speech must never self-trigger: the wake feed is
   inactive whenever Muse is speaking (turn active covers this, but
   also make sure voice.speak playback keeps it inactive — voice.speak
   runs outside a chat turn; treat "speak playing" as not-idle too, or
   document why it cannot trigger).
4. Single ownership of the microphone: the wake feed and the turn
   capture must never run at the same time. Design the handoff cleanly
   (whoever owns the audio input, the wake listener borrows it only in
   idle).
5. Add a Kconfig switch for this fork (e.g. MUSE_WAKE_WORD_HI_MUSE,
   default y for the CoreS3 overlay) so the whole feature compiles out
   cleanly. The model choice must stay "Hi Muse" — if wn10_himuse
   cannot be made to work, STOP and report why instead of substituting
   another word.
6. Size: the current app binary is about 3.4MB. If ESP-SR + the model
   do not fit the existing partition layout, you may adjust the
   partition table ONLY if dual-slot OTA keeps working; if that is not
   possible, stop and report the numbers instead of forcing it.
7. Battery/heat: WakeNet runs continuously while idle on USB power —
   that is expected for this feature. Do not add polling loops that
   spin a core; use the AFE feed task pattern from ESP-SR's examples.

## Build & commit

- Build with /tmp/build_cores3.sh until BUILD_EXIT=0, no new warnings
  in files you touch.
- Commit on cores3-custom with the message:
  "CoreS3: wake word 'Hi Muse' starts a voice turn"
- Do NOT push. Do NOT flash (no_flash).

## Report back

- The button entry point you reused, and how the wake detection calls
  it.
- How microphone ownership is handed over (idle feed vs turn capture),
  and how self-triggering during speech (chat replies and voice.speak)
  is prevented.
- Component/model versions, the final app binary size, and any
  partition change (with before/after sizes) or why none was needed.
- The cooldown and re-arm behaviour.
- Anything that can only be verified on the device (detection accuracy
  is expected to need real-world tuning — say so plainly).
