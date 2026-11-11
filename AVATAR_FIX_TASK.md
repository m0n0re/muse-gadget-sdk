# Revision task: make the StackChan-style avatar BIGGER on screen

Repo: ~/dev/muse-gadget-sdk (branch: cores3-custom). Work in esp32/.

## Background

The custom avatar at esp32/components/muse/avatar/muse_pixel.c (written
in the previous task) works, but on the real device the face looks SMALL:
the white avatar panel sits in the middle of the 320x240 screen and the
eyes/mouth occupy only a small part of the 64x64 grid (eyes 8x13, mouth
10x3). The official StackChan face fills its screen — the features are
large relative to the frame.

## Goal

Make the face read BIG, like the official one, with two levers:

1. **Enlarge the features within the 64x64 grid** (main lever):
   - Eyes: about 11 wide x 20 tall, centres around (21, 27) and (43, 27).
   - Mouth (closed): about 15 wide x 4 tall, centre around (32, 47).
   - Speaking open mouth: scale up proportionally (about 11 wide x 14
     tall at maximum).
   - Listening eyes, thinking shift, error X, closed (sleep) lines,
     happy arcs and the smile: scale them all to match the new sizes.
   - Keep everything centred and balanced; the exact numbers above are a
     starting point — tune so it looks like the official face's
     proportions (generous features, modest margins).
2. **Panel size**: check how the on-screen size of the avatar panel is
   decided (muse_pixel_set_size / muse_pixel_scale and their caller in
   the UI). If the avatar can legitimately ask for a larger panel,
   raise it so the panel nearly fills the screen height WITHOUT
   overlapping the header (READY / Wi-Fi line) or the dots below — the
   UI chrome must stay readable. If the size is fixed by the UI, leave
   it and say so in the report.

## Constraints

- Only esp32/components/muse/avatar/muse_pixel.c changes.
- Keep every state and the interface identical to the current version;
  this is a geometry pass, not a redesign.
- Build with /tmp/build_cores3.sh until BUILD_EXIT=0, no new warnings.
- Do NOT commit (gitignored by design). Do NOT flash (no_flash).

## Report back

- Final geometry numbers for eyes/mouth (idle + speaking max).
- Whether the panel size could be raised, and if so to what; if not,
  what fixes it.
- Build result.
