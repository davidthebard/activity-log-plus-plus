#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>

#include "modal_views.h"
#include "ui.h"
#include "pld.h"
#include "screens.h"
#include "sync_flow.h"
#include "title_names.h"
#include "title_db.h"
#include "audio.h"

/* ── Reset worker (used only by run_reset_view) ────────────────── */

typedef struct {
    const u32     *region_ids;
    int            region_count;
    PldFile        pld;
    PldSessionLog  sessions;
    Result         rc;
} ResetReadArgs;

static void reset_read_work(void *raw)
{
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

/* ── Detail view ───────────────────────────────────────────────── */

void run_detail_view(AppCtx *ctx, const PldSummary *game)
{
    const char *det_name = title_name_lookup(game->title_id);
    if (!det_name) det_name = title_db_lookup(game->title_id);
    char det_fallback[32];
    if (!det_name) {
        snprintf(det_fallback, sizeof(det_fallback), "0x%016llX",
                 (unsigned long long)game->title_id);
        det_name = det_fallback;
    }

    int *det_indices = malloc(ctx->sessions.count * sizeof(int));
    int  det_count = 0;
    if (det_indices) {
        for (int i = 0; i < ctx->sessions.count; i++) {
            if (ctx->sessions.entries[i].title_id == game->title_id)
                det_indices[det_count++] = i;
        }
        for (int i = 0; i < det_count - 1; i++) {
            for (int j = i + 1; j < det_count; j++) {
                if (ctx->sessions.entries[det_indices[i]].timestamp <
                    ctx->sessions.entries[det_indices[j]].timestamp) {
                    int tmp = det_indices[i];
                    det_indices[i] = det_indices[j];
                    det_indices[j] = tmp;
                }
            }
        }
    }

    int detail_scroll = 0;
    bool detail_done = false;
    bool det_hidden_toggled = false;
    nav_reset();
    while (!detail_done && aptMainLoop()) {
        audio_tick();
        hidScanInput();
        u32 dkeys = hidKeysDown();
        u32 dheld = hidKeysHeld();
        u32 dnav  = nav_tick(dkeys, dheld);
        if (dkeys & KEY_B) {
            detail_done = true;
        } else if (dkeys & KEY_X) {
            bool now_hidden = hidden_toggle(&ctx->hidden, game->title_id);
            hidden_save(&ctx->hidden);
            snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                     now_hidden ? "Hidden" : "Unhidden");
            det_hidden_toggled = true;
            detail_done = true;
        } else if (dnav & KEY_DOWN) {
            if (detail_scroll < det_count - DETAIL_VISIBLE)
                detail_scroll++;
        } else if (dnav & KEY_UP) {
            if (detail_scroll > 0)
                detail_scroll--;
        }

        if (!detail_done) {
            bool is_hidden = hidden_contains(&ctx->hidden, game->title_id);
            ui_begin_frame();
            ui_target_top();
            render_detail_top(game, det_name, &ctx->sessions,
                              det_indices, det_count,
                              detail_scroll);
            ui_target_bot();
            render_detail_bot(is_hidden);
            ui_end_frame();
        }
    }
    free(det_indices);
    if (det_hidden_toggled)
        app_ctx_rebuild(ctx);
}

/* ── Settings view ─────────────────────────────────────────────── */

void run_settings_view(AppCtx *ctx)
{
    int set_sel = 0;
    int mpi = settings_min_play_index(ctx->settings.min_play_secs);
    int svi = (int)ctx->settings.starting_view;
    if (svi < 0 || svi >= VIEW_COUNT) svi = 0;
    int music_on = ctx->settings.music_enabled ? 1 : 0;
    bool set_done = false;
    nav_reset();
    while (!set_done && aptMainLoop()) {
        audio_tick();
        hidScanInput();
        u32 skeys = hidKeysDown();
        u32 sheld = hidKeysHeld();
        u32 snav  = nav_tick(skeys, sheld);
        if (skeys & KEY_B) {
            set_done = true;
        } else if (snav & KEY_UP) {
            if (set_sel > 0) set_sel--;
        } else if (snav & KEY_DOWN) {
            if (set_sel < 2) set_sel++;
        } else if (skeys & (KEY_LEFT | KEY_RIGHT)) {
            int dir = (skeys & KEY_RIGHT) ? 1 : -1;
            if (set_sel == 0) {
                mpi = (mpi + dir + MIN_PLAY_OPTION_COUNT)
                      % MIN_PLAY_OPTION_COUNT;
            } else if (set_sel == 1) {
                svi = (svi + dir + VIEW_COUNT) % VIEW_COUNT;
            } else {
                music_on = !music_on;
            }
        }

        if (!set_done) {
            ui_begin_frame();
            ui_target_top();
            ui_draw_rect(0, 0, UI_TOP_W, UI_TOP_H, UI_COL_BG);
            ui_draw_header(UI_TOP_W);
            ui_draw_text(6, 4, UI_SCALE_HDR,
                         UI_COL_HEADER_TXT, "Settings");

            float sy = 40.0f;
            for (int r = 0; r < 3; r++) {
                float ry = sy + (float)r * 36.0f;
                u32 rbg = (r == set_sel) ? UI_COL_ROW_SEL
                        : (r % 2 == 0)   ? UI_COL_BG
                                          : UI_COL_ROW_ALT;
                ui_draw_rect(0, ry, UI_TOP_W, 32.0f, rbg);

                const char *label = (r == 0) ? "Min playtime"
                                   : (r == 1) ? "Starting view"
                                              : "Music";
                ui_draw_text(8, ry + 4, UI_SCALE_LG,
                             UI_COL_TEXT, label);

                const char *val;
                if (r == 0) {
                    val = min_play_labels[mpi];
                } else if (r == 1) {
                    val = view_labels[svi];
                } else {
                    val = music_on ? "On" : "Off";
                }
                ui_draw_text_right(UI_TOP_W - 12, ry + 4,
                                   UI_SCALE_LG,
                                   UI_COL_TEXT_DIM, val);

                if (r == set_sel) {
                    ui_draw_text(8, ry + 18, UI_SCALE_SM,
                                 UI_COL_TEXT_DIM,
                                 "< Left/Right to change >");
                }
            }

            ui_target_bot();
            ui_draw_header(UI_BOT_W);
            ui_draw_text(6, 4, UI_SCALE_HDR,
                         UI_COL_HEADER_TXT, "Settings");
            ui_draw_text_right(UI_BOT_W - 8, 36, UI_SCALE_LG,
                               UI_COL_TEXT_DIM,
                               "Up/Dn:select  L/R:change  B:save");
            ui_end_frame();
        }
    }
    ctx->settings.min_play_secs  = min_play_options[mpi];
    ctx->settings.starting_view  = (u32)svi;
    ctx->settings.music_enabled  = music_on ? 1 : 0;
    settings_save(&ctx->settings);
    audio_set_enabled(music_on);
    app_ctx_rebuild(ctx);
}

/* ── Restore view ──────────────────────────────────────────────── */

void run_restore_view(AppCtx *ctx)
{
    PldBackupList bklist;
    Result list_rc = pld_list_backups(&bklist);
    if (R_FAILED(list_rc) || bklist.count == 0) {
        snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                 bklist.count == 0 ? "No backups found"
                                   : "Error listing backups");
        return;
    }

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
        audio_tick();
        hidScanInput();
        u32 ckeys = hidKeysDown();
        u32 cheld = hidKeysHeld();
        u32 cnav  = nav_tick(ckeys, cheld);
        if (ckeys & KEY_B) {
            ctx->status_msg[0] = '\0';
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
                pld_sessions_free(&ctx->sessions);
                ctx->pld      = rst_pld;
                ctx->sessions = rst_sessions;
                ctx->view_mode = VIEW_LAST_PLAYED;
                app_ctx_rebuild(ctx);
            } else {
                pld_sessions_free(&rst_sessions);
            }

            if (R_SUCCEEDED(rst_rc))
                snprintf(ctx->status_msg, sizeof(ctx->status_msg), "Restore OK");
            else
                snprintf(ctx->status_msg, sizeof(ctx->status_msg),
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

/* ── Reset view ────────────────────────────────────────────────── */

void run_reset_view(AppCtx *ctx)
{
    bool rst_confirmed = false;
    bool rst_done = false;
    while (!rst_done && aptMainLoop()) {
        audio_tick();
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
        rr_args.region_ids    = ctx->region_ids;
        rr_args.region_count  = ctx->region_count;
        rr_args.rc = -1;
        run_loading_with_spinner("Activity Log++", "Re-reading NAND data...",
                                 reset_read_work, &rr_args);
        if (R_SUCCEEDED(rr_args.rc)) {
            pld_sessions_free(&ctx->sessions);
            ctx->pld      = rr_args.pld;
            ctx->sessions = rr_args.sessions;
            ctx->view_mode = VIEW_LAST_PLAYED;
            ctx->sync_count = 0;
            save_sync_count(0);
            app_ctx_rebuild(ctx);
            snprintf(ctx->status_msg, sizeof(ctx->status_msg), "Reset to local data");
        } else {
            snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                     "Reset failed: 0x%08lX", rr_args.rc);
        }
    }
}
