#include "title_icons.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

/* ── In-memory store (sorted by title_id ascending) ─────────────── */

static TitleIconEntry s_icons[TITLE_ICONS_MAX];
static int            s_icon_count = 0;

/* ── Binary search ───────────────────────────────────────────────── */

/* Returns index if found (>= 0), or -(insertion_point + 1) if not. */
static int bsearch_icon(u64 title_id)
{
    int lo = 0, hi = s_icon_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (s_icons[mid].title_id == title_id) return mid;
        if (s_icons[mid].title_id < title_id)  lo = mid + 1;
        else                                    hi = mid - 1;
    }
    return -(lo + 1);
}

/* ── Public: load from Morton-tiled data (ICON_TILE_BYTES) ─────── */

bool title_icon_load_from_tile_data(u64 title_id, const u16 *tile_data)
{
    if (s_icon_count >= TITLE_ICONS_MAX) return false;

    int idx = bsearch_icon(title_id);
    if (idx >= 0) return false;   /* already cached */
    int ins = -(idx + 1);

    /* Make room for the new entry in sorted position */
    memmove(&s_icons[ins + 1], &s_icons[ins],
            (size_t)(s_icon_count - ins) * sizeof(TitleIconEntry));

    TitleIconEntry *entry = &s_icons[ins];
    memset(entry, 0, sizeof(*entry));
    entry->title_id = title_id;

    /* Allocate GPU-accessible linearAlloc texture (128×128 RGB565) */
    if (!C3D_TexInit(&entry->tex, ICON_TEX_SIZE, ICON_TEX_SIZE, GPU_RGB565)) {
        /* Undo the memmove — no GPU memory was allocated */
        memmove(&s_icons[ins], &s_icons[ins + 1],
                (size_t)(s_icon_count - ins) * sizeof(TitleIconEntry));
        return false;
    }

    /* src == tex size, so tile data is a straight memcpy */
    memcpy(entry->tex.data, tile_data, ICON_TILE_BYTES);
    C3D_TexFlush(&entry->tex);

    /* Bilinear filtering for smooth downscale in list view */
    C3D_TexSetFilter(&entry->tex, GPU_LINEAR, GPU_LINEAR);

    /* UV sub-texture: full 128×128 texture */
    entry->subtex = (Tex3DS_SubTexture){
        ICON_SRC_SIZE, ICON_SRC_SIZE,
        0.0f,   /* left   */
        1.0f,   /* top    */
        1.0f,   /* right  */
        0.0f,   /* bottom */
    };
    entry->loaded = true;
    s_icon_count++;
    return true;
}

/* ── Public: SD cache save ───────────────────────────────────────── */

void title_icon_save_sd(u64 title_id, const u16 *tile_data)
{
    /* Ensure the icons cache directory exists */
    mkdir("sdmc:/3ds/activity-log-pp", 0777);   /* parent (may already exist) */
    mkdir("sdmc:/3ds/activity-log-pp/icons", 0777);

    char path[128];
    snprintf(path, sizeof(path), "%s%016llX.bin",
             ICON_CACHE_DIR, (unsigned long long)title_id);

    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(tile_data, 1, ICON_TILE_BYTES, f);
    fclose(f);
}

/* ── Public: SD cache load ───────────────────────────────────────── */

void title_icons_load_sd_cache(void)
{
    DIR *dir = opendir(ICON_CACHE_DIR);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        /* Match files of the form "0004XXXXXXXXXXXX.bin" (16 hex + ".bin") */
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len != 20) continue;                   /* 16 hex + 4 for ".bin" */
        if (strcmp(name + 16, ".bin") != 0) continue;

        /* Parse title_id from hex filename */
        char hex_buf[17];
        memcpy(hex_buf, name, 16);
        hex_buf[16] = '\0';
        char *end;
        u64 title_id = (u64)strtoull(hex_buf, &end, 16);
        if (end != hex_buf + 16) continue;         /* invalid hex */

        /* Skip if already in memory */
        if (bsearch_icon(title_id) >= 0) continue;

        /* Read tile data — reject wrong-sized files (auto-invalidates old cache) */
        char path[128];
        snprintf(path, sizeof(path), "%s%s", ICON_CACHE_DIR, name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;

        /* Check file size before allocating */
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        if (fsize != ICON_TILE_BYTES) {
            fclose(f);
            continue;   /* wrong size — old 4608-byte cache or corrupt */
        }
        fseek(f, 0, SEEK_SET);

        u16 *tile_data = (u16 *)malloc(ICON_TILE_BYTES);
        if (!tile_data) { fclose(f); continue; }

        size_t nread = fread(tile_data, 1, ICON_TILE_BYTES, f);
        fclose(f);

        if (nread == ICON_TILE_BYTES)
            title_icon_load_from_tile_data(title_id, tile_data);

        free(tile_data);
    }

    closedir(dir);
}

/* ── Public API ──────────────────────────────────────────────────── */

void title_icons_free(void)
{
    for (int i = 0; i < s_icon_count; i++) {
        if (s_icons[i].loaded)
            C3D_TexDelete(&s_icons[i].tex);
    }
    s_icon_count = 0;
}

int title_icons_count(void) { return s_icon_count; }

bool title_icon_get(u64 title_id, C2D_Image *out)
{
    int idx = bsearch_icon(title_id);
    if (idx < 0 || !s_icons[idx].loaded) return false;
    out->tex    = &s_icons[idx].tex;
    out->subtex = &s_icons[idx].subtex;
    return true;
}
