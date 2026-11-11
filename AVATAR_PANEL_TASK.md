# Task: enlarge the avatar PANEL via the CoreS3 board definition

Repo: ~/dev/muse-gadget-sdk (branch: cores3-custom). Work in esp32/.

## Background

The custom StackChan-style avatar (components/muse/avatar/muse_pixel.c)
now has large features, but on the device the white avatar PANEL is still
a centred box of about 190px on the 320x240 screen. A previous report
found the panel size is decided in components/muse/muse_ui.c
(s_canvas_px), and that a board definition can override it via
muse_board->avatar_px.

## Goal

Set avatar_px for the M5Stack CoreS3 board so the avatar panel becomes
as large as fits the screen WITHOUT overlapping the UI chrome:
the header line (Wi-Fi / READY) at the top and the two dots under the
panel must stay clear and readable.

## Steps

1. Read muse_ui.c around the s_canvas_px computation and the panel's
   placement (where the panel's origin is computed, what vertical space
   the header and dots reserve).
2. In the CoreS3 board definition (components/muse/boards/
   board_m5stack_cores3.c), set avatar_px to the largest value that
   keeps the panel clear of the header and the dots. If the placement
   math means a big panel would still overlap, choose the largest safe
   value and explain the limiting factor in the report.
3. This is a board-support change for this fork (the board file is
   tracked): COMMIT it on cores3-custom with the message:
   "CoreS3: enlarge the avatar panel (avatar_px)"
   Do NOT push.

## Constraints

- Only the CoreS3 board definition file changes. Do NOT edit muse_ui.c
  and do NOT touch the avatar renderer in this task.
- Build with /tmp/build_cores3.sh until BUILD_EXIT=0, no new warnings.
- Do NOT flash (no_flash).

## Report back

- The avatar_px value you set and the resulting panel size in pixels,
  plus the current (default) panel size for comparison.
- What limits the size (header height, dots, placement math).
- Files changed, build result, commit hash.
