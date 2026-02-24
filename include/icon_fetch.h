#pragma once
#include "pld.h"

/*
 * Download and cache cover art for titles in valid[] that have no icon.
 *
 * Game codes are resolved via the compiled-in product-code database, with
 * a fallback for DSiWare titles that encode the code in their title ID.
 *
 * Cover art is fetched from GameTDB (coverM — medium front box art),
 * centre-cropped to square, scaled to 128×128, converted to Morton-tiled
 * RGB565, loaded into the in-memory icon store, and saved to the SD
 * cache so future startups don't require internet.
 *
 * The entire phase is a no-op if Wi-Fi is not connected.
 * Per-title failures (bad URL, decode error, etc.) are silently skipped.
 */
void icon_fetch_missing(const PldSummary *const valid[], int n);
