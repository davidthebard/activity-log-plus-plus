#pragma once
#include <3ds.h>
#include <citro2d.h>

#define ICON_SRC_SIZE    128   /* GameTDB cover icon px (128×128) */
#define ICON_TEX_SIZE    128   /* POT texture size (same as src) */
#define ICON_DRAW_SIZE   48    /* display px in list row          */
#define ICON_TILE_BYTES  32768u /* 128×128×2 bytes of tiled RGB565 */
#define TITLE_ICONS_MAX  128   /* max cached icons                */

/* SD icon cache directory (no trailing slash on the path itself for mkdir,
 * but we use the slash-terminated form when building file paths). */
#define ICON_CACHE_DIR  "sdmc:/3ds/activity-log-pp/icons/"

typedef struct {
    u64               title_id;
    C3D_Tex           tex;
    Tex3DS_SubTexture subtex;
    bool              loaded;
} TitleIconEntry;

/* Load all .bin icon files from the SD cache directory into the store. */
void title_icons_load_sd_cache(void);

/* Save ICON_TILE_BYTES of Morton-tiled RGB565 icon data for title_id to SD. */
void title_icon_save_sd(u64 title_id, const u16 *tile_data);

/* Load an icon from Morton-tiled RGB565 data into the in-memory store.
 * Returns false if the store is full, the icon already exists, or GPU alloc fails. */
bool title_icon_load_from_tile_data(u64 title_id, const u16 *tile_data);

/* Free all cached textures. */
void title_icons_free(void);

/* Fill *out and return true if an icon is available for title_id. */
bool title_icon_get(u64 title_id, C2D_Image *out);

/* Return the number of icons currently held in the in-memory store. */
int title_icons_count(void);
