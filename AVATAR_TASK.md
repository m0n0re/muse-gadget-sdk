# Implementation task: StackChan-style avatar for the CoreS3 gadget

Repo: ~/dev/muse-gadget-sdk (branch: cores3-custom). Work in esp32/.

## Goal

Replace the gadget's default avatar with a face in the style of the
OFFICIAL StackChan firmware: plain white background, two black rounded
vertical eyes and a small black mouth — the classic minimal StackChan
face. The avatar is a procedural 64x64 pixel-art renderer; see
esp32/tools/muse/AVATAR_RECIPE.md for how the avatar system works.

## Before writing code, read

- tools/muse/AVATAR_RECIPE.md (the whole file).
- The DEFAULT avatar renderer and its interface (find where the default
  muse_pixel.c lives and which header declares it). Your file must be a
  drop-in replacement exposing exactly the same interface.

## Requirements

1. Write the new renderer at esp32/components/muse/avatar/muse_pixel.c
   (the recipe's location; it is gitignored upstream — leave it
   uncommitted, do NOT force-add it and do NOT edit .gitignore).
2. Design (64x64 grid, scaled up by the existing machinery):
   - Background: white. Features: black.
   - Eyes: two vertical rounded rectangles (roughly 8 wide x 13 tall,
     centred around x=22 and x=42, y around 22-35 — tune to look right).
   - Mouth: a small horizontal rounded bar (roughly 10 x 3) centred at
     x=32, y around 45.
   - Keep the proportions close to the classic StackChan face.
3. Implement every state the default renderer supports, in character:
   - boot: eyes open, neutral (a gentle blink-in is fine).
   - idle: neutral face with periodic BLINKING (eyes become thin lines
     briefly, on the default's blink cadence if it has one).
   - listening: eyes slightly wider / attentive (subtle change).
   - thinking: eyes look to one side (shifted pupils/eyes) — classic
     StackChan has no pupils, so shift the whole eyes slightly.
   - speaking: mouth alternates between the closed bar and an open
     (taller, rounded) mouth, at a natural talking cadence (the docs
     mention speaking animations — follow any level/sync hook the
     interface offers; otherwise a steady cadence).
   - error: eyes become X shapes or slanted lines (choose what reads
     clearly at 64x64).
   - off / sleep: eyes are closed lines; adding a small "Z" is optional
     (only if it renders cleanly).
   - happy (the pet reaction): eyes become upward arcs (^^) and the
     mouth a small smile arc.
   - Any other state the default implements: give it a sensible
     in-character treatment and note it in the report.
4. The first comment in the file must describe the avatar in one or two
   lines (e.g. "StackChan-style face: white background, black rounded
   eyes and mouth"), as the recipe expects.
5. No changes to any other file. The chat/TTS/speak behaviour must be
   untouched.

## Build & report

- Build with /tmp/build_cores3.sh until BUILD_EXIT=0, no new warnings in
  the file you wrote.
- Do NOT commit (the file is gitignored by design). Do NOT flash
  (mark: no_flash).
- Report back:
  - The interface you implemented (file/header you mirrored) and the
    list of states with the treatment you gave each.
  - Your final eye/mouth geometry (positions/sizes on the 64 grid).
  - Anything in the default renderer you could not mirror exactly.
  - Build result.
