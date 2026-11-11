# Fix task: wake-word build crash-loops on boot (heap corruption)

Repo: ~/dev/muse-gadget-sdk (branch: cores3-custom). Work in esp32/.

## State

- The wake-word work (9904cd1 + the voice-note revision on top) is the
  current HEAD, but the WORKING TREE has been reverted to afaba78 (the
  last known-good build) with staged changes, so the device could be
  restored. Restore the wake tree first: `git reset --hard HEAD`
  (confirm HEAD is the wake voice-note commit before resetting).
- The device is currently flashed with the known-good build and works.
  Do NOT flash anything in this task.

## Evidence (from the device's serial log, /tmp/bootlog.txt on this Mac)

The wake build **crash-loops**: ~28 boots in 45 s. Every boot proceeds
normally through audio init, then:

```
I (3170) MODEL_LOADER: The partition size is 512 KB
I (3171) MODEL_LOADER: Successfully load srmodels
assert failed: block_trim_free tlsf_control_functions.h:548 (block_is_free(block) && "block must be free")
Backtrace: 0x4038c6b5:0x3fceb4a0 0x4038c681:0x3fceb4c0 0x4038a8f5:0x3fceb4e0 0x4038ef8c:0x3fceb600 0x4038e94b:0x3fceb620 0x4037a806:0x3fceb650 0x4037a82d:0x3fceb680 0x4037a418:0x3fceb6a0 0x4200beff:0x3fceb6d0 0x421bfd7c:0x3fceb6f0 0x4200caaf:0x3fceb710 0x4209506c:0x3fceb740 0x42080145:0x3fceb760 0x420803d2:0x3fceb7f0 0x420806da:0x3fceb820 0x42026a96:0x3fceb860 0x4201b75c:0x3fceb8a0 0x420154cb:0x3fceb8d0 0x4204b5a7:0x3fceb910
ELF file SHA256: 1d2f66828
```

then `Rebooting...` with rst:0xc (RTC_SW_CPU_RST).

So: heap corruption (a TLSF block being freed/trimmed while not free —
double free or an overflow into an adjacent block), hit immediately
after the SR models load, i.e. in or around the wake listener's init /
first AFE-WakeNet setup (components/muse/muse_wake.c and its callers).

## Steps

1. `git reset --hard HEAD` to restore the wake tree. Rebuild with
   /tmp/build_cores3.sh. The resulting ELF should match the crashed
   build closely (same sources); decode the backtrace above with
   xtensa-esp32s3-elf-addr2line against the new ELF and identify the
   functions in the wake code path. (If the addresses land elsewhere,
   say where.)
2. Find the ROOT CAUSE in the wake implementation. Prime suspects —
   verify or eliminate each:
   - Buffer size/format mismatch between what idle_capture produces
     (20 ms chunks, stereo 16 kHz) and what the WakeNet/AFE feed
     consumes (frame length, channels) — an overflow writing past a
     heap buffer.
   - The detection task's stack (8 KB internal) being too small for
     WakeNet, corrupting adjacent heap.
   - AFE/model init allocating or freeing the same block twice, or
     freeing model-owned memory.
   - The stream buffer being fed before init finished, or from two
     contexts at once.
3. Fix the root cause in the code. Do NOT paper over it: no disabling
   the assert, no removing the feature, no shrinking the model.
4. Rebuild until BUILD_EXIT=0, no new warnings. Commit on cores3-custom
   with the message:
   "Fix wake-word boot crash: <the actual cause, a few words>"
   (replace the placeholder with the real cause).

## Report back

- The addr2line decoding: which of OUR functions are in the backtrace.
- The root cause, in concrete terms (what wrote/freed what, where).
- The fix, file by file.
- Which suspects you eliminated and how.
- Build result and binary size.
- Reminder in your report that this build has NOT been flashed or
  boot-tested — that happens after your report.
