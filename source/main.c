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

/* Reset: re-read NAND data */
typedef struct {
    const u32     *region_ids;
    int            region_count;
    PldFile        pld;
    PldSessionLog  sessions;
    Result         rc;
} ResetReadArgs;

static void reset_read_work(void *raw) {
    ResetReadArgs *a = (ResetReadArgs *)raw;
    a->sessions.entries = NULL;
    a->sessions.count   = 0;
    a->rc = -1;
    FS_Archive rst_archive = 0;
    for (int i = 0; i < a->region_count; i++) {
        a->rc = pld_open_archive(&rst_archive, a->region_ids[i]);
        if (R_SUCCEEDED(a->rc)) break;
    }
    if (R_FAILED(a->rc)) return;
    a->rc = pld_read_summary(rst_archive, &a->pld);
    if (R_FAILED(a->rc)) { FSUSER_CloseArchive(rst_archive); return; }
    a->rc = pld_read_sessions(rst_archive, &a->sessions);
    FSUSER_CloseArchive(rst_archive);
    if (R_FAILED(a->rc)) { pld_sessions_free(&a->sessions); return; }
    pld_backup_from_path(PLD_MERGED_PATH);
    a->rc = pld_write_sd(PLD_MERGED_PATH, &a->pld, &a->sessions);
    if (R_FAILED(a->rc)) pld_sessions_free(&a->sessions);
}

/* ── Entry point ────────────────────────────────────────────────── */

int main(void)
{
    gfxInitDefault();
    ui_init();
    fsInit();
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
        ui_fini();
        fsExit();
        gfxExit();
        return 1;
    }

    /* Step 2: Read summary + sessions */
    PldFile pld;
    PldSessionLog sessions;
    ReadPldArgs rp_args = { oa_args.archive, &pld, &sessions, -1, -1 };
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
        ui_fini();
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
        ui_fini();
        fsExit();
        gfxExit();
        return 1;
    }

    /* Step 3: Load SD merged.dat and add-only merge into NAND data */
    MergeArgs merge_args = { &pld, &sessions };
    run_with_spinner("Activity Log++", "Loading merged data...", 3, 7,
                     merge_work, &merge_args);

    u32 sync_count = load_sync_count();

    /* Step 4: Load persisted title names */
    run_with_spinner("Activity Log++", "Loading title names...", 4, 7,
                     title_names_load_work, NULL);

    /* Step 5: Scan installed titles */
    ScanNamesArgs sn_args = { 0 };
    run_with_spinner("Activity Log++", "Scanning installed titles...", 5, 7,
                     scan_names_work, &sn_args);

    /* Build valid[] before icon fetch so fetch knows which titles need icons */
    const PldSummary *valid[PLD_SUMMARY_COUNT];
    bool show_system  = false;
    bool show_unknown = false;
    ViewMode view_mode = VIEW_LAST_PLAYED;
    int n = collect_valid(&pld, valid, show_system, show_unknown);
    sort_valid(valid, n, view_mode, &sessions, &pld);

    /* Step 6: Load icon cache */
    run_with_spinner("Activity Log++", "Loading icon cache...", 6, 7,
                     title_icons_load_work, NULL);

    /* Step 7: Fetch missing icons */
    IconFetchArgs if_args = { valid, n };
    run_with_spinner("Activity Log++", "Fetching missing icons (this may take a moment)...", 7, 7,
                     icon_fetch_work, &if_args);

    int sel        = 0;
    int prev_sel   = -1;
    float sel_pop  = 0.0f;
    int scroll_top = 0;
    float scroll_y = 0.0f;

    char status_msg[48] = {0};

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
    int  list_anim_frame  = 0;
    int  rank_anim_frame  = 0;

    /* Rankings state */
    int  rank_sel      = 0;
    int  prev_rank_sel = -1;
    float rank_sel_pop = 0.0f;
    int  rank_scroll   = 0;
    const PldSummary *ranked[RANK_MAX];
    u32  rank_metric[RANK_MAX];
    int  rank_count  = 0;

    /* ── Input loop ── */
    bool quit_requested = false;
    while (!quit_requested && aptMainLoop()) {
        hidScanInput();
        u32 keys = hidKeysDown();
        u32 held = hidKeysHeld();
        u32 nav  = nav_tick(keys, held);

        if (charts_view) {
            /* ── Charts view (tabbed: pie / bar) ── */
            if (keys & KEY_B) {
                charts_view = false;
                list_anim_frame = 0;
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
                if (menu_sel < 6) menu_sel++;
            } else if (keys & KEY_B) {
                menu_open = false;
            } else if (keys & KEY_START) {
                quit_requested = true;
            } else if (keys & KEY_A) {
                switch (menu_sel) {
                    case 0: /* Charts */
                        pie_count = build_pie_data(valid, n, pie_slices, &pie_total);
                        charts_view = true;
                        chart_tab = CHART_PIE;
                        chart_anim_frame = 0;
                        menu_open = false;
                        break;

                    case 1: /* Sync */
                        run_sync_flow(&pld, &sessions, &sync_count,
                                      status_msg, sizeof(status_msg));
                        n = collect_valid(&pld, valid, show_system, show_unknown);
                        view_mode = VIEW_LAST_PLAYED;
                        sort_valid(valid, n, view_mode, &sessions, &pld);
                        {
                            IconFetchArgs psif_args = { valid, n };
                            run_loading_with_spinner("Activity Log++",
                                "Fetching missing icons (this may take a moment)...",
                                icon_fetch_work, &psif_args);
                        }
                        sel = 0; scroll_top = 0; scroll_y = 0.0f;
                        list_anim_frame = 0;
                        menu_open = false;
                        break;

                    case 2: /* Backup */
                        {
                            Result bk_rc = pld_backup_from_path(PLD_MERGED_PATH);
                            if (R_SUCCEEDED(bk_rc))
                                snprintf(status_msg, sizeof(status_msg), "Backup OK");
                            else
                                snprintf(status_msg, sizeof(status_msg),
                                         "Backup failed: 0x%08lX", bk_rc);
                        }
                        menu_open = false;
                        break;

                    case 3: /* Export */
                        {
                            ExportArgs exp_args = { &pld, &sessions, -1 };
                            run_loading_with_spinner("Activity Log++", "Exporting data...",
                                                     export_work, &exp_args);
                            if (R_SUCCEEDED(exp_args.rc))
                                snprintf(status_msg, sizeof(status_msg), "Exported to SD");
                            else
                                snprintf(status_msg, sizeof(status_msg), "Export failed");
                        }
                        menu_open = false;
                        break;

                    case 4: /* Restore */
                        {
                            PldBackupList bklist;
                            Result list_rc = pld_list_backups(&bklist);
                            if (R_FAILED(list_rc) || bklist.count == 0) {
                                snprintf(status_msg, sizeof(status_msg),
                                         bklist.count == 0 ? "No backups found"
                                                           : "Error listing backups");
                            } else {
                                int app_counts[PLD_MAX_BACKUPS] = {0};
                                for (int i = 0; i < bklist.count; i++) {
                                    char fp[128];
                                    snprintf(fp, sizeof(fp), "%s/%s",
                                             PLD_BACKUP_DIR, bklist.names[i]);
                                    pld_backup_app_count(fp, &app_counts[i]);
                                }

                                int  chooser_sel  = 0;
                                bool chooser_done = false;
                                nav_reset();
                                while (!chooser_done && aptMainLoop()) {
                                    hidScanInput();
                                    u32 ckeys = hidKeysDown();
                                    u32 cheld = hidKeysHeld();
                                    u32 cnav  = nav_tick(ckeys, cheld);
                                    if (ckeys & KEY_B) {
                                        status_msg[0] = '\0';
                                        chooser_done = true;
                                    } else if (cnav & KEY_UP) {
                                        if (chooser_sel > 0) chooser_sel--;
                                    } else if (cnav & KEY_DOWN) {
                                        if (chooser_sel < bklist.count - 1) chooser_sel++;
                                    } else if (ckeys & KEY_A) {
                                        char full_path[128];
                                        snprintf(full_path, sizeof(full_path), "%s/%s",
                                                 PLD_BACKUP_DIR, bklist.names[chooser_sel]);

                                        PldFile       rst_pld;
                                        PldSessionLog rst_sessions = {NULL, 0};
                                        Result rst_rc = pld_read_sd(full_path, &rst_pld, &rst_sessions);
                                        if (R_SUCCEEDED(rst_rc)) {
                                            rst_rc = pld_write_sd(PLD_MERGED_PATH, &rst_pld, &rst_sessions);
                                        }
                                        if (R_SUCCEEDED(rst_rc)) {
                                            pld_sessions_free(&sessions);
                                            pld      = rst_pld;
                                            sessions = rst_sessions;
                                            n = collect_valid(&pld, valid, show_system, show_unknown);
                                            view_mode = VIEW_LAST_PLAYED;
                                            sort_valid(valid, n, view_mode, &sessions, &pld);
                                            sel = 0; scroll_top = 0; scroll_y = 0.0f;
                                            list_anim_frame = 0;
                                        } else {
                                            pld_sessions_free(&rst_sessions);
                                        }

                                        if (R_SUCCEEDED(rst_rc))
                                            snprintf(status_msg, sizeof(status_msg), "Restore OK");
                                        else
                                            snprintf(status_msg, sizeof(status_msg),
                                                     "Restore failed: 0x%08lX", rst_rc);
                                        chooser_done = true;
                                    }

                                    if (!chooser_done) {
                                        ui_begin_frame();
                                        ui_target_top();
                                        ui_draw_header(UI_TOP_W);
                                        ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT,
                                                     "Restore from Backup");
                                        ui_draw_text(6, 28, UI_SCALE_SM, UI_COL_TEXT_DIM,
                                                     "Up/Down:select  A:restore  B:cancel");
                                        for (int i = 0; i < bklist.count; i++) {
                                            float ry = 46.0f + (float)i * 18.0f;
                                            u32 rbg = (i == chooser_sel) ? UI_COL_ROW_SEL :
                                                      (i % 2 == 0) ? UI_COL_BG : UI_COL_ROW_ALT;
                                            ui_draw_rect(0, ry, UI_TOP_W, 18, rbg);
                                            char label[32];
                                            fmt_backup_label(bklist.names[i], label, sizeof(label));
                                            ui_draw_textf(6, ry + 2, UI_SCALE_LG, UI_COL_TEXT,
                                                          "%s  %d apps", label, app_counts[i]);
                                        }
                                        ui_target_bot();
                                        ui_draw_header(UI_BOT_W);
                                        ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT,
                                                     "Activity Log++");
                                        ui_end_frame();
                                    }
                                }
                            }
                        }
                        menu_open = false;
                        break;

                    case 5: /* Reset */
                        {
                            bool rst_confirmed = false;
                            bool rst_done = false;
                            while (!rst_done && aptMainLoop()) {
                                hidScanInput();
                                u32 rkeys = hidKeysDown();
                                if (rkeys & KEY_A) {
                                    rst_confirmed = true;
                                    rst_done = true;
                                } else if (rkeys & (KEY_B | KEY_START)) {
                                    rst_done = true;
                                }
                                if (!rst_done) {
                                    draw_message_screen("Reset to Local",
                                        "Reset to local activity data?\n\n"
                                        "NOTE: This will remove data on\n"
                                        "this console from any synced systems\n\n"
                                        "A: confirm   B: cancel");
                                }
                            }
                            if (rst_confirmed) {
                                ResetReadArgs rr_args;
                                memset(&rr_args, 0, sizeof(rr_args));
                                rr_args.region_ids = region_ids;
                                rr_args.region_count = 4;
                                rr_args.rc = -1;
                                run_loading_with_spinner("Activity Log++", "Re-reading NAND data...",
                                                         reset_read_work, &rr_args);
                                if (R_SUCCEEDED(rr_args.rc)) {
                                    pld_sessions_free(&sessions);
                                    pld      = rr_args.pld;
                                    sessions = rr_args.sessions;
                                    n = collect_valid(&pld, valid, show_system, show_unknown);
                                    view_mode = VIEW_LAST_PLAYED;
                                    sort_valid(valid, n, view_mode, &sessions, &pld);
                                    sel = 0; scroll_top = 0; scroll_y = 0.0f;
                                    list_anim_frame = 0;
                                    sync_count = 0;
                                    save_sync_count(0);
                                    snprintf(status_msg, sizeof(status_msg), "Reset to local data");
                                } else {
                                    snprintf(status_msg, sizeof(status_msg),
                                             "Reset failed: 0x%08lX", rr_args.rc);
                                }
                            }
                        }
                        menu_open = false;
                        break;

                    case 6: /* Quit */
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
                if (!show_system && !show_unknown) {
                    show_system = true;  show_unknown = false;
                } else if (show_system && !show_unknown) {
                    show_system = true;  show_unknown = true;
                } else {
                    show_system = false; show_unknown = false;
                }
                n = collect_valid(&pld, valid, show_system, show_unknown);
                if (view_is_rank(view_mode)) {
                    rank_count = build_rankings(valid, n, view_mode, &sessions, &pld,
                                                ranked, rank_metric);
                    rank_sel = 0; rank_scroll = 0;
                    rank_anim_frame = 0;
                } else {
                    sort_valid(valid, n, view_mode, &sessions, &pld);
                    sel = 0; scroll_top = 0; scroll_y = 0.0f;
                    list_anim_frame = 0;
                }
                status_msg[0] = '\0';
            } else if (keys & KEY_L) {
                view_mode = (view_mode + VIEW_COUNT - 1) % VIEW_COUNT;
                if (view_is_rank(view_mode)) {
                    rank_count = build_rankings(valid, n, view_mode, &sessions, &pld,
                                                ranked, rank_metric);
                    rank_sel = 0; rank_scroll = 0;
                    rank_anim_frame = 0;
                } else {
                    sort_valid(valid, n, view_mode, &sessions, &pld);
                    sel = 0; scroll_top = 0; scroll_y = 0.0f;
                    list_anim_frame = 0;
                }
                status_msg[0] = '\0';
            } else if (keys & KEY_R) {
                view_mode = (view_mode + 1) % VIEW_COUNT;
                if (view_is_rank(view_mode)) {
                    rank_count = build_rankings(valid, n, view_mode, &sessions, &pld,
                                                ranked, rank_metric);
                    rank_sel = 0; rank_scroll = 0;
                    rank_anim_frame = 0;
                } else {
                    sort_valid(valid, n, view_mode, &sessions, &pld);
                    sel = 0; scroll_top = 0; scroll_y = 0.0f;
                    list_anim_frame = 0;
                }
                status_msg[0] = '\0';
            }

            /* Navigation with hold-to-repeat */
            if (view_is_rank(view_mode)) {
                if (nav & KEY_DOWN) {
                    if (rank_sel < rank_count - 1) {
                        rank_sel++;
                        if (rank_sel >= rank_scroll + UI_VISIBLE_ROWS)
                            rank_scroll = rank_sel - UI_VISIBLE_ROWS + 1;
                    }
                } else if (nav & KEY_UP) {
                    if (rank_sel > 0) {
                        rank_sel--;
                        if (rank_sel < rank_scroll)
                            rank_scroll = rank_sel;
                    }
                }
            } else {
                if (nav & KEY_DOWN) {
                    if (sel < n - 1) {
                        sel++;
                        if (sel >= scroll_top + UI_VISIBLE_ROWS)
                            scroll_top = sel - UI_VISIBLE_ROWS + 1;
                    }
                } else if (nav & KEY_UP) {
                    if (sel > 0) {
                        sel--;
                        if (sel < scroll_top)
                            scroll_top = sel;
                    }
                }
            }

            /* Detail screen */
            {
                const PldSummary *det_s = NULL;
                if (view_is_rank(view_mode)) {
                    if ((keys & KEY_A) && rank_count > 0)
                        det_s = ranked[rank_sel];
                } else {
                    if ((keys & KEY_A) && n > 0)
                        det_s = valid[sel];
                }
                if (det_s) {
                    const char *det_name = title_name_lookup(det_s->title_id);
                    if (!det_name) det_name = title_db_lookup(det_s->title_id);
                    char det_fallback[32];
                    if (!det_name) {
                        snprintf(det_fallback, sizeof(det_fallback), "0x%016llX",
                                 (unsigned long long)det_s->title_id);
                        det_name = det_fallback;
                    }

                    int *det_indices = malloc(sessions.count * sizeof(int));
                    int  det_count = 0;
                    if (det_indices) {
                        for (int i = 0; i < sessions.count; i++) {
                            if (sessions.entries[i].title_id == det_s->title_id)
                                det_indices[det_count++] = i;
                        }
                        for (int i = 0; i < det_count - 1; i++) {
                            for (int j = i + 1; j < det_count; j++) {
                                if (sessions.entries[det_indices[i]].timestamp <
                                    sessions.entries[det_indices[j]].timestamp) {
                                    int tmp = det_indices[i];
                                    det_indices[i] = det_indices[j];
                                    det_indices[j] = tmp;
                                }
                            }
                        }
                    }

                    int detail_scroll = 0;
                    bool detail_done = false;
                    nav_reset();
                    while (!detail_done && aptMainLoop()) {
                        hidScanInput();
                        u32 dkeys = hidKeysDown();
                        u32 dheld = hidKeysHeld();
                        u32 dnav  = nav_tick(dkeys, dheld);
                        if (dkeys & KEY_B) {
                            detail_done = true;
                        } else if (dnav & KEY_DOWN) {
                            if (detail_scroll < det_count - DETAIL_VISIBLE)
                                detail_scroll++;
                        } else if (dnav & KEY_UP) {
                            if (detail_scroll > 0)
                                detail_scroll--;
                        }

                        if (!detail_done) {
                            ui_begin_frame();
                            ui_target_top();
                            render_detail_top(det_s, det_name, &sessions,
                                              det_indices, det_count,
                                              detail_scroll);
                            ui_target_bot();
                            render_detail_bot();
                            ui_end_frame();
                        }
                    }
                    free(det_indices);
                }
            }
        }

        if (!charts_view) {
            if (view_is_rank(view_mode)) {
                if (rank_sel != prev_rank_sel) { rank_sel_pop = 0.0f; prev_rank_sel = rank_sel; }
                rank_sel_pop = lerpf(rank_sel_pop, 1.0f, 0.25f);
                if (rank_sel_pop > 0.99f) rank_sel_pop = 1.0f;

                float rank_anim_t = (float)rank_anim_frame / 40.0f;
                if (rank_anim_t > 2.0f) rank_anim_t = 2.0f;
                rank_anim_frame++;

                ui_begin_frame();
                ui_target_top();
                render_rankings_top(ranked, rank_count, rank_sel, rank_scroll,
                                    rank_metric, view_mode, rank_anim_t,
                                    rank_sel_pop);
                if (menu_open) render_menu(menu_sel);
                ui_target_bot();
                render_bottom_stats(valid, n, &sessions, sync_count,
                                    status_msg, show_system, show_unknown);
                ui_end_frame();
            } else {
                float scroll_target = (float)scroll_top * UI_ROW_PITCH;
                scroll_y = lerpf(scroll_y, scroll_target, 0.3f);
                if (scroll_y - scroll_target < 0.5f && scroll_y - scroll_target > -0.5f)
                    scroll_y = scroll_target;

                if (sel != prev_sel) { sel_pop = 0.0f; prev_sel = sel; }
                sel_pop = lerpf(sel_pop, 1.0f, 0.25f);
                if (sel_pop > 0.99f) sel_pop = 1.0f;

                float list_anim_t = (float)list_anim_frame / 40.0f;
                if (list_anim_t > 2.0f) list_anim_t = 2.0f;
                list_anim_frame++;

                ui_begin_frame();
                ui_target_top();
                render_game_list(valid, n, sel, scroll_y, &sessions, status_msg,
                                 show_system, show_unknown, view_mode, list_anim_t,
                                 sel_pop);
                if (menu_open) render_menu(menu_sel);
                ui_target_bot();
                render_bottom_stats(valid, n, &sessions, sync_count,
                                    status_msg, show_system, show_unknown);
                ui_end_frame();
            }
        }
    }

    pld_sessions_free(&sessions);
    title_icons_free();
    title_names_free();
    ui_fini();
    fsExit();
    gfxExit();
    return 0;
}
