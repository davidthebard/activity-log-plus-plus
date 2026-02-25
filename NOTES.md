# Development Notes

## GPU calls from worker threads (background spinner)

### Context

The background spinner (`run_with_spinner`) spawns a worker thread for blocking
I/O while the main thread renders a loading animation. Two callsites run code
that performs GPU operations from the worker thread:

- **Step 6** (`title_icons_load_sd_cache`) — reads .bin files from SD, then
  calls `title_icon_load_from_tile_data()` which does `C3D_TexInit`,
  `C3D_TexFlush`, and `C3D_TexSetFilter`.
- **Step 7 / post-sync** (`icon_fetch_missing`) — downloads JPEGs over HTTP,
  decodes and tiles them, then calls the same `title_icon_load_from_tile_data()`.

The C3D texture functions are documented as main-thread-only by convention.

### Why it works anyway

1. **Core 1 path**: When the worker runs on core 1, its `C3D_TexInit` /
   `C3D_TexFlush` calls are CPU-side memory operations (`linearAlloc` + cache
   flush). They don't submit to the GPU command buffer that the render thread
   uses, so there's no actual contention.

2. **Core 0 fallback path**: When both threads share core 0, the worker only
   gets CPU time during VSync waits inside `ui_end_frame()`. At that point the
   render frame is already submitted, so texture allocations happen between
   frames.

3. **No concurrent texture reads**: The spinner screen never calls
   `title_icon_get()`. The textures created by the worker aren't sampled until
   after the spinner loop exits and the main game list starts drawing. There is
   no overlap between creation and use.

### Why we're not fixing it now

The issue is "unsafe by API contract" but safe by construction given the current
control flow. A proper fix would split `title_icon_load_from_tile_data()` into a
worker-safe "buffer tile data" step and a main-thread "upload to GPU" step. The
slow part (disk/network I/O, JPEG decode, tiling) is already worker-safe; the
GPU upload is a near-instant memcpy + cache flush, so the split would add
complexity for no visible benefit.

### If this ever breaks

Symptoms would be corrupted textures, crashes in `C3D_TexInit`, or graphical
glitches on the game list screen after startup. The fix is to defer GPU texture
creation: have the worker produce raw tile data buffers, then have the main
thread call `title_icon_load_from_tile_data()` after the spinner loop returns.

### Related: title_names.c global state

`title_names_load()`, `title_names_scan_installed()`, and `title_names_save()`
mutate static globals (`s_entries[]`, `s_count`). These run on the worker thread
during startup steps 4-5 and during sync. The render thread only reads this
state via `title_name_lookup()` in the main game list loop, which doesn't run
during spinner screens. Same pattern: safe by construction, not by API contract.

---

## SMDH icon extraction (attempted, not yet working)

### Goal

Many titles that appear in the Activity Log lack cover art on GameTDB — notably
all system apps (Mii Maker, StreetPass Mii Plaza, etc.) and some eShop-only
games. Every installed 3DS title has an SMDH file (`/icon.bin` in the ExeFS
content archive) that contains a 48×48 Morton-tiled RGB565 icon at offset
`0x24C0` (0x1200 bytes). See https://www.3dbrew.org/wiki/SMDH for the format.

The existing `title_names_scan_installed()` in `title_names.c` already
successfully opens and reads this same SMDH file (for title names) using
`FSUSER_OpenFileDirectly` with archive ID `0x2345678A`
(`ARCHIVE_SAVEDATA_AND_CONTENT`). The idea was to also extract the large icon
from the same file and feed it into the GPU icon store as a fallback when
GameTDB has no cover art.

### What was tried

1. **Direct offset read, iterating `valid[]`.**
   Added `read_smdh_large_icon()` using `FSFILE_Read` at offset `0x24C0` to
   grab just the icon bytes. Called from a new `icon_scan_smdh()` that looped
   over the filtered `valid[]` array after `icon_fetch_missing()`.
   Result: no SMDH icons appeared.

2. **Direct offset read, iterating full PLD summaries.**
   `valid[]` is filtered by `show_system` (default false), so system apps were
   never passed to the scan. Changed `icon_scan_smdh` to iterate
   `pld->summaries[PLD_SUMMARY_COUNT]` directly, skipping empty entries.
   Result: no SMDH icons appeared.

3. **Full-file read from offset 0, iterating full PLD summaries.**
   Suspected that `FSFILE_Read` at a non-zero offset might not work reliably
   through the ExeFS content archive. Changed to read the full 0x36C0-byte
   SMDH from offset 0 (matching the proven `read_title_name` pattern), then
   `memcpy` the icon out of the buffer. Added SMDH magic verification.
   Result: no SMDH icons appeared.

4. **Full-file read with AM enumeration (mirroring `title_names_scan_installed`).**
   Suspected the archive might require an active AM session or that the media
   type must come from `AM_GetTitleList` (not guessed). Rewrote
   `icon_scan_smdh` to call `amInit()`, enumerate installed titles via
   `AM_GetTitleCount`/`AM_GetTitleList` per media type, then read each title's
   SMDH — the exact same flow as `title_names_scan_installed`.
   Result: no SMDH icons appeared.

### GPU texture loading (64×64)

A new function `title_icon_load_from_smdh_tile()` was written to load the 48×48
icon into a 64×64 GPU texture (smallest POT >= 48). It zeroed the texture,
re-mapped tiles from 6-tile row stride to 8-tile row stride, and set sub-texture
UVs to `(0, 0)-(0.75, 0.75)`. The SD cache was updated to accept both 32768-byte
(GameTDB 128×128) and 4608-byte (SMDH 48×48) files.

This code was never validated in isolation because the SMDH data never reached
it — or if it did, no icons appeared on screen. Without on-device diagnostics it
is unclear whether the failure is in the read, the GPU load, or the UV mapping.

### Why it might not be working

Since `read_title_name` uses the identical archive-access pattern and works, the
failure is puzzling. Possible explanations:

- **Worker-thread context.** `title_names_scan_installed` runs at step 5 on a
  worker thread, and it works. `icon_scan_smdh` runs at step 7 on a different
  worker thread invocation. Maybe some process-global state (FS session handle,
  AM session registration) established during step 5 is no longer valid by
  step 7, and re-initializing AM doesn't fully replicate it.

- **`C3D_TexInit` with 64×64 textures.** The existing code only creates 128×128
  textures. If 64×64 `C3D_TexInit` silently fails or produces a texture the
  renderer can't sample, the icon would never appear. This was never tested
  independently.

- **Sub-texture UV correctness.** The 128×128 icons use UVs `(0,0)-(1,1)`. The
  64×64 icons used `(0,0)-(0.75,0.75)`. If the citro2d UV convention for
  `Tex3DS_SubTexture` doesn't work as assumed, the icon might render as
  transparent or off-screen pixels.

- **Icon store capacity.** `TITLE_ICONS_MAX` is 128. If GameTDB already fills
  most slots, SMDH icons would silently fail to insert. Unlikely to explain
  *zero* icons, but worth checking.

- **Silent early return.** If `amInit()` fails at step 7 (service session limit,
  or the worker thread can't open a new session), the entire function returns
  immediately with no visible error.

### Ideas for future attempts

1. **Add on-device diagnostics.** Return a count from `icon_scan_smdh` (or write
   a small log file to SD) showing how many titles were enumerated, how many
   SMDH reads succeeded, how many GPU loads succeeded. This would immediately
   isolate which stage is failing.

2. **Piggyback on `title_names_scan_installed`.** Instead of a separate scan,
   extend `read_title_name` to read the full 0x36C0-byte SMDH and save the
   48×48 icon data to the SD icon cache right there — in the exact context
   that's proven to work. Then `title_icons_load_sd_cache` at step 6 would
   load them on the next startup. This eliminates all concerns about AM
   session state, worker-thread context, and timing.

3. **Upscale to 128×128 and use proven GPU path.** Bypass the untested 64×64
   texture code entirely. Un-tile the 48×48 Morton data to flat RGB888,
   nearest-neighbor scale to 128×128, re-tile with `rgb888_to_smdh_tile`, and
   load with the existing `title_icon_load_from_tile_data`. More wasteful
   (32 KB vs 8 KB per icon) but uses the exact GPU code path that GameTDB
   icons already prove works.

4. **Test `C3D_TexInit` with 64×64 independently.** Create a solid-color 64×64
   texture at startup and draw it somewhere visible. If it renders correctly,
   the GPU path is fine and the problem is in the SMDH reading. If it doesn't
   render, the problem is in the 64×64 texture or UV setup.

5. **Use a different SMDH source.** Instead of `ARCHIVE_SAVEDATA_AND_CONTENT`,
   try reading SMDH data via `AM_GetTitleInfo` or by opening the title's CXI
   directly through `ARCHIVE_ROMFS` or `ARCHIVE_EXTDATA`. The 3dbrew wiki
   notes that the SMDH icon encoding could vary (RGBA8, RGB565, ETC1, etc.)
   though RGB565 is the only format observed in practice.
