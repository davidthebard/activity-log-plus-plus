#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <3ds.h>

#include "pld.h"
#include "ui.h"
#include "title_db.h"
#include "title_names.h"
#include "title_icons.h"
#include "icon_fetch.h"
#include "screens.h"
#include "charts.h"
#include "export.h"
#include "sync_flow.h"
#include "render_views.h"
#include "settings.h"
#include "app_ctx.h"
#include "modal_views.h"
#include "audio.h"

/* ── Constants ──────────────────────────────────────────────────── */

static const u32 region_ids[] = {
    ACTIVITY_SAVE_ID_USA,
    ACTIVITY_SAVE_ID_EUR,
    ACTIVITY_SAVE_ID_JPN,
    ACTIVITY_SAVE_ID_KOR,
};

/* ── Worker arg structs and functions ──────────────────────────── */

/* Step 1: Open archive */
typedef struct {
    const u32 *region_ids;
    int        region_count;
    FS_Archive archive;
    Result     rc;
} OpenArchiveArgs;

static void open_archive_work(void *raw) {
    OpenArchiveArgs *a = (OpenArchiveArgs *)raw;
    a->rc = -1;
    for (int i = 0; i < a->region_count; i++) {
        a->rc = pld_open_archive(&a->archive, a->region_ids[i]);
        if (R_SUCCEEDED(a->rc)) break;
    }
}

/* Step 2: Read summary + sessions */
typedef struct {
    FS_Archive     archive;
    PldFile       *pld;
    PldSessionLog *sessions;
    Result         rc_summary;
    Result         rc_sessions;
} ReadPldArgs;

static void read_pld_work(void *raw) {
    ReadPldArgs *a = (ReadPldArgs *)raw;
    a->rc_summary = pld_read_summary(a->archive, a->pld);
    if (R_FAILED(a->rc_summary)) {
        a->rc_sessions = -1;
        return;
    }
    a->rc_sessions = pld_read_sessions(a->archive, a->sessions);
}

/* Step 3: Merge */
typedef struct {
    PldFile       *pld;
    PldSessionLog *sessions;
} MergeArgs;

static void merge_work(void *raw) {
    MergeArgs *a = (MergeArgs *)raw;
    PldFile       sd_pld;
    PldSessionLog sd_sessions = {NULL, 0};
    mkdir(PLD_BACKUP_DIR, 0777);
    if (R_SUCCEEDED(pld_read_sd(PLD_MERGED_PATH, &sd_pld, &sd_sessions))) {
        pld_merge_sessions(a->sessions, &sd_sessions, true);
        pld_merge_summaries(a->pld, sd_pld.summaries, sd_pld.summary_count, true);
        pld_sessions_free(&sd_sessions);
        for (int i = 0; i < PLD_SUMMARY_COUNT; i++) {
            PldSummary *s = &a->pld->summaries[i];
            if (pld_summary_is_empty(s)) continue;
            s->total_secs = 0;
            for (int j = 0; j < a->sessions->count; j++)
                if (a->sessions->entries[j].title_id == s->title_id)
                    s->total_secs += a->sessions->entries[j].play_secs;
        }
    }
    pld_write_sd(PLD_MERGED_PATH, a->pld, a->sessions);
}

/* Step 4: title_names_load */
static void title_names_load_work(void *arg) {
    (void)arg;
    title_names_load();
}

/* Step 5: Scan installed titles */
typedef struct {
    int new_names;
} ScanNamesArgs;

static void scan_names_work(void *raw) {
    ScanNamesArgs *a = (ScanNamesArgs *)raw;
    a->new_names = title_names_scan_installed();
    if (a->new_names > 0) title_names_save();
}

/* Step 6: title_icons_load_sd_cache */
static void title_icons_load_work(void *arg) {
    (void)arg;
    title_icons_load_sd_cache();
}

/* Step 7 + post-sync: icon_fetch_missing */
typedef struct {
    const PldSummary *const *valid;
    int n;
} IconFetchArgs;

static void icon_fetch_work(void *raw) {
    IconFetchArgs *a = (IconFetchArgs *)raw;
    icon_fetch_missing(a->valid, a->n);
}

/* ── Entry point ────────────────────────────────────────────────── */

int main(void)
{
    gfxInitDefault();
    ui_init();
    fsInit();
    romfsInit();
    APT_SetAppCpuTimeLimit(30);

    /* Step 1: Open save archive */
    OpenArchiveArgs oa_args = { region_ids, 4, 0, -1 };
    run_with_spinner("Activity Log++", "Opening save archive...", 1, 7,
                     open_archive_work, &oa_args);
    if (R_FAILED(oa_args.rc)) {
        char err_body[96];
        snprintf(err_body, sizeof(err_body),
                 "Error: 0x%08lX\n\nIs CFW active and Activity Log used?\n\nPress START to exit.",
                 oa_args.rc);
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            draw_message_screen("Error", err_body);
        }
        audio_exit();
        ui_fini();
        romfsExit();
        fsExit();
        gfxExit();
        return 1;
    }

    /* Step 2: Read summary + sessions */
    AppCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.region_ids   = region_ids;
    ctx.region_count = 4;

    ReadPldArgs rp_args = { oa_args.archive, &ctx.pld, &ctx.sessions, -1, -1 };
    run_with_spinner("Activity Log++", "Reading pld.dat...", 2, 7,
                     read_pld_work, &rp_args);
    FSUSER_CloseArchive(oa_args.archive);
    if (R_FAILED(rp_args.rc_summary)) {
        char err_body[80];
        snprintf(err_body, sizeof(err_body),
                 "Error reading summary: 0x%08lX\n\nPress START to exit.",
                 rp_args.rc_summary);
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            draw_message_screen("Error", err_body);
        }
        audio_exit();
        ui_fini();
        romfsExit();
        fsExit();
        gfxExit();
        return 1;
    }
    if (R_FAILED(rp_args.rc_sessions)) {
        char err_body[80];
        snprintf(err_body, sizeof(err_body),
                 "Error reading sessions: 0x%08lX\n\nPress START to exit.",
                 rp_args.rc_sessions);
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            draw_message_screen("Error", err_body);
        }
        audio_exit();
        ui_fini();
        romfsExit();
        fsExit();
        gfxExit();
        return 1;
    }

    /* Step 3: Load SD merged.dat and add-only merge into NAND data */
    MergeArgs merge_args = { &ctx.pld, &ctx.sessions };
    run_with_spinner("Activity Log++", "Loading merged data...", 3, 7,
                     merge_work, &merge_args);

    ctx.sync_count = load_sync_count();

    /* Step 4: Load persisted title names */
    run_with_spinner("Activity Log++", "Loading title names...", 4, 7,
                     title_names_load_work, NULL);

    /* Step 5: Scan installed titles */
    ScanNamesArgs sn_args = { 0 };
    run_with_spinner("Activity Log++", "Scanning installed titles...", 5, 7,
                     scan_names_work, &sn_args);

    /* Load user settings and hidden-games list */
    settings_load(&ctx.settings);
    hidden_load(&ctx.hidden);
    ctx.view_mode = (ViewMode)ctx.settings.starting_view;
    if (ctx.view_mode >= VIEW_COUNT) ctx.view_mode = VIEW_LAST_PLAYED;

    /* Build valid[] before icon fetch so fetch knows which titles need icons */
    app_ctx_rebuild(&ctx);

    /* Step 6: Load icon cache */
    run_with_spinner("Activity Log++", "Loading icon cache...", 6, 7,
                     title_icons_load_work, NULL);

    /* Step 7: Fetch missing icons */
    IconFetchArgs if_args = { ctx.valid, ctx.n };
    run_with_spinner("Activity Log++", "Fetching missing icons (this may take a moment)...", 7, 7,
                     icon_fetch_work, &if_args);

    /* Start background music after all setup is complete */
    audio_init("romfs:/bgm.mp3");
    audio_set_enabled(ctx.settings.music_enabled != 0);

    int prev_sel   = -1;
    float sel_pop  = 0.0f;

    bool menu_open = false;
    int  menu_sel  = 0;

    /* Charts state */
    typedef enum { CHART_PIE, CHART_BAR, CHART_TAB_COUNT } ChartTab;
    bool charts_view = false;
    ChartTab chart_tab = CHART_PIE;
    PieSlice pie_slices[PIE_SLICES + 1];
    int  pie_count = 0;
    u32  pie_total = 0;
    int  chart_anim_frame = 0;

    /* Rankings animation state */
    int  prev_rank_sel = -1;
    float rank_sel_pop = 0.0f;

    /* ── Input loop ── */
    bool quit_requested = false;
    while (!quit_requested && aptMainLoop()) {
        audio_tick();
        hidScanInput();
        u32 keys = hidKeysDown();
        u32 held = hidKeysHeld();
        u32 nav  = nav_tick(keys, held);

        if (charts_view) {
            /* ── Charts view (tabbed: pie / bar) ── */
            if (keys & KEY_B) {
                charts_view = false;
                ctx.list_anim_frame = 0;
            } else if (keys & KEY_L) {
                chart_tab = (chart_tab + CHART_TAB_COUNT - 1) % CHART_TAB_COUNT;
                chart_anim_frame = 0;
            } else if (keys & KEY_R) {
                chart_tab = (chart_tab + 1) % CHART_TAB_COUNT;
                chart_anim_frame = 0;
            }

            float anim_t = (float)chart_anim_frame / 40.0f;
            if (anim_t > 3.0f) anim_t = 3.0f;
            chart_anim_frame++;

            ui_begin_frame();
            ui_target_top();
            if (chart_tab == CHART_BAR)
                render_bar_top(pie_slices, pie_count, pie_total, anim_t);
            else
                render_pie_top(pie_slices, pie_count, pie_total, anim_t);
            ui_target_bot();
            render_pie_bot(pie_slices, pie_count, pie_total, anim_t);
            ui_end_frame();
        } else if (menu_open) {
            /* ── Menu open: navigate and confirm ── */
            if (keys & KEY_UP) {
                if (menu_sel > 0) menu_sel--;
            } else if (keys & KEY_DOWN) {
                if (menu_sel < 7) menu_sel++;
            } else if (keys & KEY_B) {
                menu_open = false;
            } else if (keys & KEY_START) {
                quit_requested = true;
            } else if (keys & KEY_A) {
                switch (menu_sel) {
                    case 0: /* Charts */
                        pie_count = build_pie_data(ctx.valid, ctx.n,
                                                   pie_slices, &pie_total);
                        charts_view = true;
                        chart_tab = CHART_PIE;
                        chart_anim_frame = 0;
                        menu_open = false;
                        break;

                    case 1: /* Sync */
                        run_sync_flow(&ctx.pld, &ctx.sessions, &ctx.sync_count,
                                      ctx.status_msg, sizeof(ctx.status_msg));
                        ctx.view_mode = VIEW_LAST_PLAYED;
                        app_ctx_rebuild(&ctx);
                        {
                            IconFetchArgs psif_args = { ctx.valid, ctx.n };
                            run_loading_with_spinner("Activity Log++",
                                "Fetching missing icons (this may take a moment)...",
                                icon_fetch_work, &psif_args);
                        }
                        menu_open = false;
                        break;

                    case 2: /* Backup */
                        {
                            Result bk_rc = pld_backup_from_path(PLD_MERGED_PATH);
                            if (R_SUCCEEDED(bk_rc))
                                snprintf(ctx.status_msg, sizeof(ctx.status_msg),
                                         "Backup OK");
                            else
                                snprintf(ctx.status_msg, sizeof(ctx.status_msg),
                                         "Backup failed: 0x%08lX", bk_rc);
                        }
                        menu_open = false;
                        break;

                    case 3: /* Export */
                        {
                            ExportArgs exp_args = { &ctx.pld, &ctx.sessions, -1 };
                            run_loading_with_spinner("Activity Log++",
                                "Exporting data...",
                                export_work, &exp_args);
                            if (R_SUCCEEDED(exp_args.rc))
                                snprintf(ctx.status_msg, sizeof(ctx.status_msg),
                                         "Exported to SD");
                            else
                                snprintf(ctx.status_msg, sizeof(ctx.status_msg),
                                         "Export failed");
                        }
                        menu_open = false;
                        break;

                    case 4: /* Restore */
                        run_restore_view(&ctx);
                        menu_open = false;
                        break;

                    case 5: /* Reset */
                        run_reset_view(&ctx);
                        menu_open = false;
                        break;

                    case 6: /* Settings */
                        run_settings_view(&ctx);
                        menu_open = false;
                        break;

                    case 7: /* Quit */
                        quit_requested = true;
                        break;
                }
            }
        } else {
            /* ── Menu closed: viewer navigation ── */
            if (keys & KEY_START) {
                menu_open = true;
                menu_sel  = 0;
            }

            else if (keys & KEY_Y) {
                if (!ctx.show_system && !ctx.show_unknown) {
                    ctx.show_system = true;  ctx.show_unknown = false;
                } else if (ctx.show_system && !ctx.show_unknown) {
                    ctx.show_system = true;  ctx.show_unknown = true;
                } else {
                    ctx.show_system = false; ctx.show_unknown = false;
                }
                app_ctx_rebuild(&ctx);
                ctx.status_msg[0] = '\0';
            } else if (keys & KEY_L) {
                ctx.view_mode = (ctx.view_mode + VIEW_COUNT - 1) % VIEW_COUNT;
                app_ctx_rebuild(&ctx);
                ctx.status_msg[0] = '\0';
            } else if (keys & KEY_R) {
                ctx.view_mode = (ctx.view_mode + 1) % VIEW_COUNT;
                app_ctx_rebuild(&ctx);
                ctx.status_msg[0] = '\0';
            }

            /* Navigation with hold-to-repeat */
            if (view_is_rank(ctx.view_mode)) {
                if (nav & KEY_DOWN) {
                    if (ctx.rank_sel < ctx.rank_count - 1) {
                        ctx.rank_sel++;
                        if (ctx.rank_sel >= ctx.rank_scroll + UI_VISIBLE_ROWS)
                            ctx.rank_scroll = ctx.rank_sel - UI_VISIBLE_ROWS + 1;
                    }
                } else if (nav & KEY_UP) {
                    if (ctx.rank_sel > 0) {
                        ctx.rank_sel--;
                        if (ctx.rank_sel < ctx.rank_scroll)
                            ctx.rank_scroll = ctx.rank_sel;
                    }
                }
            } else {
                if (nav & KEY_DOWN) {
                    if (ctx.sel < ctx.n - 1) {
                        ctx.sel++;
                        if (ctx.sel >= ctx.scroll_top + UI_VISIBLE_ROWS)
                            ctx.scroll_top = ctx.sel - UI_VISIBLE_ROWS + 1;
                    }
                } else if (nav & KEY_UP) {
                    if (ctx.sel > 0) {
                        ctx.sel--;
                        if (ctx.sel < ctx.scroll_top)
                            ctx.scroll_top = ctx.sel;
                    }
                }
            }

            /* Detail screen */
            {
                const PldSummary *det_s = NULL;
                if (view_is_rank(ctx.view_mode)) {
                    if ((keys & KEY_A) && ctx.rank_count > 0)
                        det_s = ctx.ranked[ctx.rank_sel];
                } else {
                    if ((keys & KEY_A) && ctx.n > 0)
                        det_s = ctx.valid[ctx.sel];
                }
                if (det_s)
                    run_detail_view(&ctx, det_s);
            }
        }

        if (!charts_view) {
            if (view_is_rank(ctx.view_mode)) {
                if (ctx.rank_sel != prev_rank_sel) {
                    rank_sel_pop = 0.0f;
                    prev_rank_sel = ctx.rank_sel;
                }
                rank_sel_pop = lerpf(rank_sel_pop, 1.0f, 0.25f);
                if (rank_sel_pop > 0.99f) rank_sel_pop = 1.0f;

                float rank_anim_t = (float)ctx.rank_anim_frame / 40.0f;
                if (rank_anim_t > 2.0f) rank_anim_t = 2.0f;
                ctx.rank_anim_frame++;

                ui_begin_frame();
                ui_target_top();
                render_rankings_top(ctx.ranked, ctx.rank_count, ctx.rank_sel,
                                    ctx.rank_scroll, ctx.rank_metric,
                                    ctx.view_mode, rank_anim_t,
                                    rank_sel_pop);
                if (menu_open) render_menu(menu_sel);
                ui_target_bot();
                render_bottom_stats(ctx.valid, ctx.n, &ctx.sessions,
                                    ctx.sync_count, ctx.status_msg,
                                    ctx.show_system, ctx.show_unknown);
                ui_end_frame();
            } else {
                float scroll_target = (float)ctx.scroll_top * UI_ROW_PITCH;
                ctx.scroll_y = lerpf(ctx.scroll_y, scroll_target, 0.3f);
                if (ctx.scroll_y - scroll_target < 0.5f &&
                    ctx.scroll_y - scroll_target > -0.5f)
                    ctx.scroll_y = scroll_target;

                if (ctx.sel != prev_sel) {
                    sel_pop = 0.0f;
                    prev_sel = ctx.sel;
                }
                sel_pop = lerpf(sel_pop, 1.0f, 0.25f);
                if (sel_pop > 0.99f) sel_pop = 1.0f;

                float list_anim_t = (float)ctx.list_anim_frame / 40.0f;
                if (list_anim_t > 2.0f) list_anim_t = 2.0f;
                ctx.list_anim_frame++;

                ui_begin_frame();
                ui_target_top();
                render_game_list(ctx.valid, ctx.n, ctx.sel, ctx.scroll_y,
                                 &ctx.sessions, ctx.status_msg,
                                 ctx.show_system, ctx.show_unknown,
                                 ctx.view_mode, list_anim_t, sel_pop);
                if (menu_open) render_menu(menu_sel);
                ui_target_bot();
                render_bottom_stats(ctx.valid, ctx.n, &ctx.sessions,
                                    ctx.sync_count, ctx.status_msg,
                                    ctx.show_system, ctx.show_unknown);
                ui_end_frame();
            }
        }
    }

    pld_sessions_free(&ctx.sessions);
    title_icons_free();
    title_names_free();
    audio_exit();
    ui_fini();
    romfsExit();
    fsExit();
    gfxExit();
    return 0;
}
