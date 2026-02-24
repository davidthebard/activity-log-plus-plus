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
