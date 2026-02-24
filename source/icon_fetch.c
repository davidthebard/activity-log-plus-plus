/*
 * icon_fetch.c — Download missing game cover art from GameTDB via httpc.
 *
 * Image decoding (JPEG/PNG) uses stb_image (source/vendor/stb_image.h).
 * Cover art is centre-cropped and scaled to 128×128, then converted to
 * Morton-tiled RGB565 for the GPU icon store.
 */
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image.h"

#include "icon_fetch.h"
#include "title_icons.h"
#include "product_code_db.h"
#include "pld.h"

#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* GameTDB cover-art URL template.  coverM (medium front box art) is the
 * smallest available artwork type — typically ~10-40 KB JPEG.  HTTPS is
 * required because the server redirects plain HTTP, and the 3DS HTTPC
 * service does not follow redirects automatically. */
#define FETCH_BASE_URL    "https://art.gametdb.com/3ds/coverM/"
#define FETCH_BUF_SIZE    (128 * 1024)  /* 128 KB — covers are larger than icons */
#define FETCH_MAX_REDIRS  3             /* follow at most this many 301/302s */

/* ── RGB888 flat → Morton-tiled RGB565 ──────────────────────────── */

/*
 * Top-left-crop the largest square from the source, then nearest-neighbour
 * scale to a flat ICON_SRC_SIZE×ICON_SRC_SIZE RGB888 buffer.
 * For portrait box art the square covers the top of the image (title area).
 */
static void rgb888_crop_scale(const unsigned char *src, int w, int h,
                               unsigned char dst[ICON_SRC_SIZE * ICON_SRC_SIZE * 3])
{
    int side = (w < h) ? w : h;
    int ox   = 0;
    int oy   = 0;

    for (int y = 0; y < ICON_SRC_SIZE; y++) {
        int sy = oy + y * side / ICON_SRC_SIZE;
        for (int x = 0; x < ICON_SRC_SIZE; x++) {
            int sx = ox + x * side / ICON_SRC_SIZE;
            const unsigned char *s = src + (sy * w + sx) * 3;
            unsigned char       *d = dst + (y  * ICON_SRC_SIZE + x) * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
    }
}

/*
 * Convert a flat ICON_SRC_SIZE×ICON_SRC_SIZE RGB888 image into
 * Morton-tiled RGB565 format used by title_icons.c.
 */
static void rgb888_to_smdh_tile(const unsigned char *src_rgb888, u16 *dst_tile)
{
    for (int y = 0; y < ICON_SRC_SIZE; y++) {
        for (int x = 0; x < ICON_SRC_SIZE; x++) {
            const unsigned char *px = src_rgb888 + (y * ICON_SRC_SIZE + x) * 3;

            /* RGB888 → RGB565 */
            u16 r = ((u16)px[0] >> 3) & 0x1Fu;
            u16 g = ((u16)px[1] >> 2) & 0x3Fu;
            u16 b = ((u16)px[2] >> 3) & 0x1Fu;
            u16 rgb565 = (r << 11) | (g << 5) | b;

            /* Morton index within 8×8 tile */
            int px8 = x % 8, py8 = y % 8;
            u32 m = (u32)(px8 & 1)          |
                    (u32)((py8 & 1) << 1)   |
                    (u32)((px8 & 2) << 1)   |
                    (u32)((py8 & 2) << 2)   |
                    (u32)((px8 & 4) << 2)   |
                    (u32)((py8 & 4) << 3);

            /* Tile row stride = ICON_SRC_SIZE / 8 */
            int tx = x / 8, ty = y / 8;
            dst_tile[(tx + ty * (ICON_SRC_SIZE / 8)) * 64 + (int)m] = rgb565;
        }
    }
}

/* ── Game code derivation ────────────────────────────────────────── */

/*
 * Derive a GameTDB-compatible 4-character product code for DSiWare titles.
 *
 * DSiWare title IDs (upper 32 bits == 0x00048004) encode the product code
 * as big-endian ASCII in the lower 32 bits (e.g. 0x484E4841 → "HNHA").
 *
 * Retail 3DS games (0x00040000) use a numeric unique ID with no relation
 * to the GameTDB product code, so they are explicitly excluded.  Without a
 * separate title-ID→code lookup table there is no way to resolve them here.
 *
 * Returns true and fills code_out[5] on success, false otherwise.
 */
static bool derive_game_code(u64 title_id, char code_out[5])
{
    /* Primary: compiled-in lookup table covers retail 3DS and DSiWare */
    const char *db_code = product_code_db_lookup(title_id);
    if (db_code) {
        memcpy(code_out, db_code, 4);
        code_out[4] = '\0';
        return true;
    }

    /* Fallback: DSiWare encodes its product code as ASCII in the lower 32 bits
     * (e.g. title_id low = 0x484E4841 → "HNHA").  Useful before the database
     * has been generated, or for titles absent from the source JSON files. */
    u32 hi = (u32)(title_id >> 32);
    if (hi != 0x00048004u) return false;

    u32 lo = (u32)(title_id & 0xFFFFFFFFu);
    code_out[0] = (char)((lo >> 24) & 0xFF);
    code_out[1] = (char)((lo >> 16) & 0xFF);
    code_out[2] = (char)((lo >>  8) & 0xFF);
    code_out[3] = (char)((lo >>  0) & 0xFF);
    code_out[4] = '\0';

    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)code_out[i];
        if (c < 0x20u || c > 0x7Eu) return false;
    }
    return true;
}

/* ── Public API ──────────────────────────────────────────────────── */

void icon_fetch_missing(const PldSummary *const valid[], int n)
{
    if (n <= 0) return;

    /* Quick Wi-Fi connectivity check — skip the whole phase if offline */
    if (R_FAILED(acInit())) return;
    u32 wifi_status = 0;
    ACU_GetWifiStatus(&wifi_status);
    acExit();
    if (wifi_status == 0) return;   /* not connected to any Wi-Fi network */

    if (R_FAILED(httpcInit(0))) return;

    u8 *fetch_buf = (u8 *)malloc(FETCH_BUF_SIZE);
    if (!fetch_buf) {
        httpcExit();
        return;
    }

    for (int i = 0; i < n; i++) {
        u64 title_id = valid[i]->title_id;

        /* Skip if we already have an icon for this title */
        C2D_Image dummy;
        if (title_icon_get(title_id, &dummy)) continue;

        /* Skip if we can't derive a printable ASCII game code */
        char code[5];
        if (!derive_game_code(title_id, code)) continue;

        /* Build URL — try US region first (broadest catalogue on GameTDB) */
        char cur_url[256];
        snprintf(cur_url, sizeof(cur_url), "%sUS/%s.jpg", FETCH_BASE_URL, code);

        bool request_ok = false;
        httpcContext ctx;
        u32 downloaded = 0;

        for (int redir = 0; redir <= FETCH_MAX_REDIRS; redir++) {
            if (R_FAILED(httpcOpenContext(&ctx, HTTPC_METHOD_GET, cur_url, 0)))
                break;

            httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
            httpcAddRequestHeaderField(&ctx, "User-Agent",
                                       "activity-log-pp/1.0");

            if (R_FAILED(httpcBeginRequest(&ctx))) {
                httpcCloseContext(&ctx);
                break;
            }

            u32 status_code = 0;
            if (R_FAILED(httpcGetResponseStatusCode(&ctx, &status_code))) {
                httpcCloseContext(&ctx);
                break;
            }

            if (status_code == 200) {
                Result dl_rc = httpcDownloadData(&ctx, fetch_buf,
                                                 FETCH_BUF_SIZE, &downloaded);
                httpcCloseContext(&ctx);
                if (R_SUCCEEDED(dl_rc) && downloaded > 0)
                    request_ok = true;
                break;
            }

            if ((status_code == 301 || status_code == 302 ||
                 status_code == 303 || status_code == 307) &&
                redir < FETCH_MAX_REDIRS) {
                char location[256];
                Result hdr_rc = httpcGetResponseHeader(
                    &ctx, "Location", location, sizeof(location));
                httpcCloseContext(&ctx);
                if (R_FAILED(hdr_rc)) break;
                strncpy(cur_url, location, sizeof(cur_url) - 1);
                cur_url[sizeof(cur_url) - 1] = '\0';
                continue;
            }

            httpcCloseContext(&ctx);
            break;
        }

        if (!request_ok) continue;

        /* Decode image (JPEG or PNG) — forces 3-channel RGB output */
        int w = 0, h = 0, ch = 0;
        stbi_uc *pixels = stbi_load_from_memory(
            fetch_buf, (int)downloaded, &w, &h, &ch, STBI_rgb);
        if (!pixels || w <= 0 || h <= 0) continue;

        /* Top-left-crop + scale to ICON_SRC_SIZE × ICON_SRC_SIZE */
        unsigned char *scaled = (unsigned char *)malloc(ICON_SRC_SIZE * ICON_SRC_SIZE * 3);
        if (!scaled) { stbi_image_free(pixels); continue; }
        rgb888_crop_scale(pixels, w, h, scaled);

        /* Convert flat RGB888 → Morton-tiled RGB565 */
        u16 *tile_data = (u16 *)malloc(ICON_TILE_BYTES);
        if (!tile_data) { free(scaled); stbi_image_free(pixels); continue; }
        rgb888_to_smdh_tile(scaled, tile_data);
        stbi_image_free(pixels);
        free(scaled);

        /* Load into in-memory icon store and persist to SD cache */
        title_icon_load_from_tile_data(title_id, tile_data);
        title_icon_save_sd(title_id, tile_data);
        free(tile_data);
    }

    free(fetch_buf);
    httpcExit();
}
