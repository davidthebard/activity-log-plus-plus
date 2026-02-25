#include <stdio.h>
#include <string.h>
#include <3ds.h>

#include "sync_flow.h"
#include "screens.h"
#include "net.h"
#include "pld.h"
#include "title_names.h"
#include "audio.h"

#define SYNC_COUNT_PATH  "sdmc:/3ds/activity-log-pp/synccount"

/* ── Sync counter helpers ────────────────────────────────────────── */

u32 load_sync_count(void) {
    u32 n = 0;
    FILE *f = fopen(SYNC_COUNT_PATH, "rb");
    if (f) { fread(&n, sizeof(n), 1, f); fclose(f); }
    return n;
}

void save_sync_count(u32 n) {
    FILE *f = fopen(SYNC_COUNT_PATH, "wb");
    if (f) { fwrite(&n, sizeof(n), 1, f); fclose(f); }
}

/* ── Worker arg structs ──────────────────────────────────────────── */

typedef struct {
    NetCtx *ctx;
    NetRole role;
    Result  rc;
} NetInitArgs;

static void net_init_work(void *raw) {
    NetInitArgs *a = (NetInitArgs *)raw;
    a->rc = net_init(a->ctx, a->role);
}

typedef struct {
    NetCtx        *ctx;
    PldSessionLog *sessions;
    int            new_sess;
    int            rc;
} NetExchSessionsArgs;

static void net_exch_sessions_work(void *raw) {
    NetExchSessionsArgs *a = (NetExchSessionsArgs *)raw;
    a->rc = net_exchange_sessions(a->ctx, a->sessions, &a->new_sess);
}

typedef struct {
    NetCtx  *ctx;
    PldFile *pld;
    int      new_apps;
    int      rc;
} NetExchSummariesArgs;

static void net_exch_summaries_work(void *raw) {
    NetExchSummariesArgs *a = (NetExchSummariesArgs *)raw;
    a->rc = net_exchange_summaries(a->ctx, a->pld, &a->new_apps);
}

typedef struct {
    NetCtx *ctx;
    int     rc;
} NetExchNamesArgs;

static void net_exch_names_work(void *raw) {
    NetExchNamesArgs *a = (NetExchNamesArgs *)raw;
    a->rc = net_exchange_title_names(a->ctx);
    if (a->rc == 0) title_names_save();
}

/* ── Sync flow ──────────────────────────────────────────────────── */

void run_sync_flow(PldFile *pld, PldSessionLog *sessions,
                   u32 *sync_count, char *status_msg, int status_msg_len)
{
    NetCtx net_ctx;
    memset(&net_ctx, 0, sizeof(net_ctx));
    net_ctx.tcp_sock = net_ctx.listen_sock = net_ctx.udp_sock = -1;
    bool net_active = false;

    while (aptMainLoop()) {
        audio_tick();
        hidScanInput();
        u32 role_keys = hidKeysDown();

        if (role_keys & (KEY_X | KEY_Y | KEY_B)) {
            if (role_keys & KEY_B) return;
            NetRole role = (role_keys & KEY_X) ? NET_ROLE_HOST : NET_ROLE_CLIENT;
            NetInitArgs ni_args = { &net_ctx, role, -1 };
            run_loading_with_spinner("Activity Log++", "Initializing network...",
                                     net_init_work, &ni_args);
            if (R_FAILED(ni_args.rc)) {
                char err_body[80];
                snprintf(err_body, sizeof(err_body),
                         "Network init failed: 0x%08lX\n\nPress START to continue.",
                         ni_args.rc);
                while (aptMainLoop()) {
                    audio_tick();
                    hidScanInput();
                    if (hidKeysDown() & KEY_START) break;
                    draw_message_screen("Network Error", err_body);
                }
                return;
            }
            net_active = true;
            break;
        }

        draw_message_screen("Activity Log++",
                            "Connect to another 3DS?\n\nX: Host\nY: Client\nB: Back");
    }

    if (!net_active) return;

    {
        int connected_frames = 0;
        NetState prev_state = (NetState)-1;
        char prev_peer_ip[16] = {0};
        char net_title[64] = "Connecting...";
        char net_body[192] = "";

        while (aptMainLoop()) {
            audio_tick();
            hidScanInput();
            u32 net_keys = hidKeysDown();

            if (net_ctx.state != NET_STATE_CONNECTED &&
                net_ctx.state != NET_STATE_ERROR) {
                if (net_keys & KEY_START) {
                    net_shutdown(&net_ctx);
                    return;
                }
                net_tick(&net_ctx);
            } else if (net_ctx.state == NET_STATE_ERROR) {
                if (net_keys & KEY_START) {
                    net_shutdown(&net_ctx);
                    return;
                }
            } else {
                connected_frames++;
                if (connected_frames >= 120) break;
            }

            bool net_changed = (net_ctx.state != prev_state) ||
                               (strcmp(net_ctx.peer_ip, prev_peer_ip) != 0);
            if (net_changed) {
                if (net_ctx.state == NET_STATE_CONNECTED) {
                    snprintf(net_title, sizeof(net_title), "Connected!");
                    snprintf(net_body, sizeof(net_body),
                             "Peer: %s", net_ctx.peer_ip);
                } else if (net_ctx.state == NET_STATE_ERROR) {
                    snprintf(net_title, sizeof(net_title), "Network Error");
                    snprintf(net_body, sizeof(net_body),
                             "Press START to continue.");
                } else if (net_ctx.role == NET_ROLE_HOST) {
                    snprintf(net_title, sizeof(net_title), "HOST");
                    snprintf(net_body, sizeof(net_body),
                             "Own IP: %s\nBroadcasting...\nWaiting for client\n\nSTART: cancel",
                             net_ctx.own_ip);
                } else {
                    snprintf(net_title, sizeof(net_title), "CLIENT");
                    if (net_ctx.peer_ip[0] != '\0')
                        snprintf(net_body, sizeof(net_body),
                                 "Connecting to %s...\n\nSTART: cancel",
                                 net_ctx.peer_ip);
                    else
                        snprintf(net_body, sizeof(net_body),
                                 "Scanning for host...\n\nSTART: cancel");
                }
                prev_state = net_ctx.state;
                snprintf(prev_peer_ip, sizeof(prev_peer_ip), "%s",
                         net_ctx.peer_ip);
            }

            draw_message_screen_ex(net_title, net_body,
                                   net_ctx.state != NET_STATE_ERROR);
        }
    }

    if (net_ctx.state != NET_STATE_CONNECTED) {
        net_shutdown(&net_ctx);
        return;
    }

    int sess_rc = -1, app_rc = -1;
    int new_sess = 0, new_apps = 0;

    {
        NetExchSessionsArgs es_args = { &net_ctx, sessions, 0, -1 };
        run_loading_with_spinner("Syncing...", "Exchanging sessions...",
                                 net_exch_sessions_work, &es_args);
        sess_rc = es_args.rc;
        new_sess = es_args.new_sess;
    }

    if (sess_rc == 0) {
        NetExchSummariesArgs ea_args = { &net_ctx, pld, 0, -1 };
        run_loading_with_spinner("Syncing...", "Syncing app list...",
                                 net_exch_summaries_work, &ea_args);
        app_rc = ea_args.rc;
        new_apps = ea_args.new_apps;
    }

    if (sess_rc == 0 && app_rc == 0) {
        NetExchNamesArgs en_args = { &net_ctx, -1 };
        run_loading_with_spinner("Syncing...", "Exchanging title names...",
                                 net_exch_names_work, &en_args);
    }

    if (sess_rc == 0 && app_rc == 0) {
        for (int i = 0; i < PLD_SUMMARY_COUNT; i++) {
            PldSummary *s = &pld->summaries[i];
            if (pld_summary_is_empty(s)) continue;
            s->total_secs = 0;
            for (int j = 0; j < sessions->count; j++) {
                if (sessions->entries[j].title_id == s->title_id)
                    s->total_secs += sessions->entries[j].play_secs;
            }
        }
    }

    if (sess_rc == 0 && app_rc == 0) {
        char sync_body[64];
        snprintf(sync_body, sizeof(sync_body),
                 "+%d sessions, +%d apps", new_sess, new_apps);
        snprintf(status_msg, (size_t)status_msg_len,
                 "Synced: +%d sess +%d apps", new_sess, new_apps);
        for (int f = 0; f < 120 && aptMainLoop(); f++) {
            hidScanInput();
            draw_message_screen("Sync Complete", sync_body);
        }

        pld_backup_from_path(PLD_MERGED_PATH);
        Result sd_rc = pld_write_sd(PLD_MERGED_PATH, pld, sessions);
        if (R_FAILED(sd_rc)) {
            snprintf(status_msg, (size_t)status_msg_len, "SD save failed");
        } else {
            (*sync_count)++;
            save_sync_count(*sync_count);
        }
    } else {
        snprintf(status_msg, (size_t)status_msg_len, "Sync failed");
        for (int f = 0; f < 120 && aptMainLoop(); f++) {
            hidScanInput();
            draw_message_screen("Sync Failed", "Continuing with local data.");
        }
    }

    net_shutdown(&net_ctx);
}
