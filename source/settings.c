#include <stdio.h>
#include <string.h>
#include "settings.h"

/* ── Min-play option tables ────────────────────────────────────── */

const u32 min_play_options[MIN_PLAY_OPTION_COUNT] = {
    60, 300, 600, 1800, 3600
};

const char *min_play_labels[MIN_PLAY_OPTION_COUNT] = {
    "1 min", "5 min", "10 min", "30 min", "1 hour"
};

/* ── AppSettings ───────────────────────────────────────────────── */

void settings_defaults(AppSettings *s)
{
    s->magic         = SETTINGS_MAGIC;
    s->min_play_secs = 600;
    s->starting_view = 0;   /* VIEW_LAST_PLAYED */
    s->music_enabled = 1;
}

void settings_load(AppSettings *s)
{
    settings_defaults(s);
    FILE *f = fopen(SETTINGS_PATH, "rb");
    if (!f) return;
    AppSettings tmp;
    if (fread(&tmp, sizeof(tmp), 1, f) == 1 && tmp.magic == SETTINGS_MAGIC) {
        *s = tmp;
    }
    fclose(f);
}

void settings_save(const AppSettings *s)
{
    FILE *f = fopen(SETTINGS_PATH, "wb");
    if (!f) return;
    fwrite(s, sizeof(*s), 1, f);
    fclose(f);
}

int settings_min_play_index(u32 secs)
{
    for (int i = 0; i < MIN_PLAY_OPTION_COUNT; i++) {
        if (min_play_options[i] == secs) return i;
    }
    return 2; /* default: 10 min */
}

/* ── HiddenGames ───────────────────────────────────────────────── */

void hidden_load(HiddenGames *h)
{
    h->count = 0;
    FILE *f = fopen(HIDDEN_PATH, "rb");
    if (!f) return;
    u32 cnt = 0;
    if (fread(&cnt, sizeof(cnt), 1, f) != 1 || cnt > MAX_HIDDEN) {
        fclose(f);
        return;
    }
    if (fread(h->title_ids, sizeof(u64), cnt, f) == cnt)
        h->count = (int)cnt;
    fclose(f);
}

void hidden_save(const HiddenGames *h)
{
    FILE *f = fopen(HIDDEN_PATH, "wb");
    if (!f) return;
    u32 cnt = (u32)h->count;
    fwrite(&cnt, sizeof(cnt), 1, f);
    fwrite(h->title_ids, sizeof(u64), h->count, f);
    fclose(f);
}

bool hidden_contains(const HiddenGames *h, u64 title_id)
{
    for (int i = 0; i < h->count; i++) {
        if (h->title_ids[i] == title_id) return true;
    }
    return false;
}

bool hidden_toggle(HiddenGames *h, u64 title_id)
{
    for (int i = 0; i < h->count; i++) {
        if (h->title_ids[i] == title_id) {
            h->title_ids[i] = h->title_ids[h->count - 1];
            h->count--;
            return false;
        }
    }
    if (h->count < MAX_HIDDEN) {
        h->title_ids[h->count++] = title_id;
    }
    return true;
}
