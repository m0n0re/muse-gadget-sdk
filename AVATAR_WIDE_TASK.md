# Implementation task: wide (full-width) avatar face for the CoreS3 gadget

Repo: ~/dev/muse-gadget-sdk (branch: cores3-custom). Work in esp32/.

## Goal

The gadget's avatar is currently a SQUARE panel (about 176x176) centred
on the 320x240 screen. The user wants the face to span the full WIDTH of
the screen, like the official StackChan firmware's face: a wide
rectangular face area, 320 wide and roughly 180-200 tall, between the
header (Wi-Fi / READY line) and the page dots. The StackChan-style face
(white background, black rounded eyes and mouth, in
components/muse/avatar/muse_pixel.c) must be redrawn for the wide area.

Do NOT stretch the square rendering horizontally — the face must be
drawn natively on a wide canvas.

## Before writing code, read

- components/muse/muse_ui.c — how the avatar panel size/position is
  computed (s_canvas_px, placement), where captions and other overlays
  sit relative to the panel, and every place the avatar is drawn.
- components/muse/muse_pixel.h — the renderer interface
  (muse_pixel_render, muse_pixel_set_size, muse_pixel_scale, accents).
- The default renderer (esp32/avatar/muse_pixel.c) and our custom one
  (components/muse/avatar/muse_pixel.c).
- The CoreS3 board definition (the avatar_px = 176 override from commit
  a765a5d, reapplied in 62fc84d).

## Requirements

1. Rectangular canvas for the MAIN face view:
   - Width: the full screen width (320 on CoreS3).
   - Height: the space between the header and the page dots — derive
     the exact value from the layout code so neither the header
     (Wi-Fi/READY, status labels incl. the 16px Japanese font case) nor
     the dots are overlapped.
   - Keep the change as contained as the codebase allows. Extending the
     renderer interface to carry width and height is acceptable;
     changing the default renderer's behaviour for other boards is not
     (other boards / other screens keep working as before, with the
     square layout as the fallback when a board does not opt in).
   - The CoreS3 board opts in. Reconcile the existing avatar_px
     override with the new mechanism (supersede it cleanly; say what
     you did in the report).
2. Redraw the StackChan-style face on the wide grid:
   - Keep the cell size the current renderer uses (so the pixel look is
     unchanged); the grid becomes wider than tall (e.g. 80 x ~48).
   - Eyes spread wider apart, sized in proportion to the official
     StackChan face on a 320-wide screen (in cells, roughly: eyes 9
     wide x 17 tall, centres near x=26 and x=54 of 80; mouth about 13
     wide x 3 tall, centred, lower third). Tune to look right.
   - Adapt EVERY state to the wide canvas: boot, idle + blinking,
     listening, thinking (eye shift), working scan, speaking (mouth
     synced via muse_speaking as now), error (X eyes), off/sleep
     (closed lines + Z), happy (^^ arcs + smile).
3. Captions and overlays: if the chat caption or any overlay is
   positioned relative to the square panel, adapt it to the wide area
   so text stays readable and inside the screen. Note what you changed.
4. Any OTHER screen that draws the avatar must still work (square
   fallback is fine there). List them in the report.
5. Chat / TTS / voice.speak behaviour must be untouched.

## Build & commit

- Build with /tmp/build_cores3.sh until BUILD_EXIT=0, no new warnings
  in files you touch.
- Tracked-file changes: commit on cores3-custom with the message:
  "CoreS3: wide full-width avatar face"
  The custom avatar file itself stays uncommitted (gitignored by
  design). Do NOT push. Do NOT flash (no_flash).

## Report back

- The final face area in pixels (w x h, position) and the grid size.
- How the interface/layout changed (files, mechanism, opt-in), and what
  happened to the avatar_px override.
- Every other avatar call site and how it behaves now.
- Final eye/mouth geometry on the wide grid.
- Anything you could not verify without the device.
