#pragma once

#include <3ds.h>
#include <stdbool.h>

/* ── Paths ──────────────────────────────────────────────────────── */

#define SETTINGS_PATH  "sdmc:/3ds/activity-log-pp/settings.dat"
#define HIDDEN_PATH    "sdmc:/3ds/activity-log-pp/hidden.dat"

/* ── App settings ───────────────────────────────────────────────── */

#define SETTINGS_MAGIC  0x414C5053u   /* "ALPS" */

#define MIN_PLAY_OPTION_COUNT 5

extern const u32  min_play_options[MIN_PLAY_OPTION_COUNT];
extern const char *min_play_labels[MIN_PLAY_OPTION_COUNT];

typedef struct {
    u32 magic;
    u32 min_play_secs;   /* default 600 (10 min) */
    u32 starting_view;   /* ViewMode enum value   */
} AppSettings;

void settings_defaults(AppSettings *s);
void settings_load(AppSettings *s);
void settings_save(const AppSettings *s);

/* Return the index into min_play_options matching secs, or the
 * default index (2 = 10 min) if not found. */
int  settings_min_play_index(u32 secs);

/* ── Hidden games ───────────────────────────────────────────────── */

#define MAX_HIDDEN 256

typedef struct {
    u64 title_ids[MAX_HIDDEN];
    int count;
} HiddenGames;

void hidden_load(HiddenGames *h);
void hidden_save(const HiddenGames *h);
bool hidden_contains(const HiddenGames *h, u64 title_id);

/* Toggle title_id: add if absent, remove if present.
 * Returns true if the title is now hidden, false if unhidden. */
bool hidden_toggle(HiddenGames *h, u64 title_id);
