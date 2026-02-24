#include "net.h"          /* must come first — pulls in <3ds.h> for SOC service */
#include "title_names.h"  /* TitleNameEntry, title_names_get_all, title_names_merge */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <malloc.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static u32 *s_soc_buf = NULL;

/* ── Helpers ──────────────────────────────────────────────────────── */

static void set_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void close_sock(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* Receive exactly len bytes from a blocking socket; returns len on success,
 * 0 on clean close, -1 on error. MSG_WAITALL is unreliable on 3DS SOC. */
static int recv_all(int fd, void *buf, int len)
{
    int total = 0;
    while (total < len) {
        int n = recv(fd, (char *)buf + total, len - total, 0);
        if (n <= 0) return n;
        total += n;
    }
    return total;
}

static int send_all(int fd, const void *buf, int len)
{
    int total = 0;
    while (total < len) {
        int n = send(fd, (const char *)buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return total;
}

/* ── net_init ─────────────────────────────────────────────────────── */

Result net_init(NetCtx *ctx, NetRole role)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->tcp_sock    = -1;
    ctx->listen_sock = -1;
    ctx->udp_sock    = -1;
    ctx->role        = role;

    s_soc_buf = (u32 *)memalign(0x1000, NET_SOC_BUF_SIZE);
    if (!s_soc_buf) return -1;

    Result rc = socInit(s_soc_buf, NET_SOC_BUF_SIZE);
    if (R_FAILED(rc)) {
        free(s_soc_buf);
        s_soc_buf = NULL;
        return rc;
    }

    /* Own IP from gethostid() */
    struct in_addr own_addr;
    own_addr.s_addr = gethostid();
    snprintf(ctx->own_ip, sizeof(ctx->own_ip), "%s", inet_ntoa(own_addr));

    if (role == NET_ROLE_HOST) {
        /* UDP socket for broadcasting discovery */
        ctx->udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (ctx->udp_sock < 0) { socExit(); free(s_soc_buf); s_soc_buf = NULL; return -1; }

        int bcast = 1;
        setsockopt(ctx->udp_sock, SOL_SOCKET, SO_BROADCAST,
                   &bcast, sizeof(bcast));
        set_nonblocking(ctx->udp_sock);

        /* TCP listening socket */
        ctx->listen_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (ctx->listen_sock < 0) {
            close_sock(&ctx->udp_sock);
            socExit(); free(s_soc_buf); s_soc_buf = NULL;
            return -1;
        }

        struct sockaddr_in srv = {0};
        srv.sin_family      = AF_INET;
        srv.sin_addr.s_addr = INADDR_ANY;
        srv.sin_port        = htons(NET_TCP_PORT);
        if (bind(ctx->listen_sock, (struct sockaddr *)&srv, sizeof(srv)) < 0 ||
            listen(ctx->listen_sock, 1) < 0) {
            close_sock(&ctx->listen_sock);
            close_sock(&ctx->udp_sock);
            socExit(); free(s_soc_buf); s_soc_buf = NULL;
            return -1;
        }
        set_nonblocking(ctx->listen_sock);

        ctx->state       = NET_STATE_WAITING;
        ctx->bcast_timer = 60; /* broadcast on first tick */

    } else {
        /* CLIENT: UDP socket bound to discovery port to receive broadcasts */
        ctx->udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (ctx->udp_sock < 0) { socExit(); free(s_soc_buf); s_soc_buf = NULL; return -1; }

        struct sockaddr_in bind_addr = {0};
        bind_addr.sin_family      = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port        = htons(NET_UDP_PORT);
        if (bind(ctx->udp_sock, (struct sockaddr *)&bind_addr,
                 sizeof(bind_addr)) < 0) {
            close_sock(&ctx->udp_sock);
            socExit(); free(s_soc_buf); s_soc_buf = NULL;
            return -1;
        }
        set_nonblocking(ctx->udp_sock);

        ctx->state = NET_STATE_SCANNING;
    }

    return 0;
}

/* ── net_tick ─────────────────────────────────────────────────────── */

void net_tick(NetCtx *ctx)
{
    if (ctx->state == NET_STATE_CONNECTED || ctx->state == NET_STATE_ERROR)
        return;

    if (ctx->role == NET_ROLE_HOST) {
        /* Broadcast discovery magic every ~60 frames */
        ctx->bcast_timer++;
        if (ctx->bcast_timer >= 60) {
            ctx->bcast_timer = 0;
            u32 magic = NET_MAGIC;
            struct sockaddr_in bcast_addr = {0};
            bcast_addr.sin_family      = AF_INET;
            bcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            bcast_addr.sin_port        = htons(NET_UDP_PORT);
            sendto(ctx->udp_sock, &magic, 4, 0,
                   (struct sockaddr *)&bcast_addr, sizeof(bcast_addr));
        }

        /* Check for incoming TCP connections */
        struct sockaddr_in peer_addr = {0};
        socklen_t peer_len = sizeof(peer_addr);
        int fd = accept(ctx->listen_sock,
                        (struct sockaddr *)&peer_addr, &peer_len);
        if (fd >= 0) {
            snprintf(ctx->peer_ip, sizeof(ctx->peer_ip), "%s",
                     inet_ntoa(peer_addr.sin_addr));
            /* Accepted socket may inherit O_NONBLOCK from listen_sock on 3DS;
             * clear it so the blocking handshake recv_all works correctly. */
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
            ctx->tcp_sock = fd;

            /* Handshake: send magic, recv magic */
            u32 magic = NET_MAGIC;
            send(ctx->tcp_sock, &magic, 4, 0);

            u32 rx = 0;
            int got = recv_all(ctx->tcp_sock, &rx, 4);
            if (got == 4 && rx == NET_MAGIC) {
                ctx->state = NET_STATE_CONNECTED;
                close_sock(&ctx->listen_sock);
            } else {
                close_sock(&ctx->tcp_sock);
                /* stay in WAITING */
            }
        }

    } else {
        /* CLIENT: listen for UDP broadcast from host */
        u32 rx = 0;
        struct sockaddr_in from = {0};
        socklen_t fromlen = sizeof(from);
        int got = recvfrom(ctx->udp_sock, &rx, 4, 0,
                           (struct sockaddr *)&from, &fromlen);
        if (got == 4 && rx == NET_MAGIC) {
            snprintf(ctx->peer_ip, sizeof(ctx->peer_ip), "%s",
                     inet_ntoa(from.sin_addr));

            /* Connect TCP to host */
            int tcp = socket(AF_INET, SOCK_STREAM, 0);
            if (tcp < 0) { ctx->state = NET_STATE_ERROR; return; }

            struct sockaddr_in host_addr = {0};
            host_addr.sin_family = AF_INET;
            host_addr.sin_port   = htons(NET_TCP_PORT);
            inet_aton(ctx->peer_ip, &host_addr.sin_addr);

            if (connect(tcp, (struct sockaddr *)&host_addr,
                        sizeof(host_addr)) < 0) {
                close(tcp);
                ctx->state = NET_STATE_ERROR;
                return;
            }
            ctx->tcp_sock = tcp;

            /* Handshake: recv magic, send magic */
            u32 rx2 = 0;
            int got2 = recv_all(ctx->tcp_sock, &rx2, 4);
            if (got2 == 4 && rx2 == NET_MAGIC) {
                u32 magic = NET_MAGIC;
                send(ctx->tcp_sock, &magic, 4, 0);
                ctx->state = NET_STATE_CONNECTED;
                close_sock(&ctx->udp_sock);
            } else {
                close_sock(&ctx->tcp_sock);
                /* stay in SCANNING */
            }
        }
    }
}

/* ── net_shutdown ─────────────────────────────────────────────────── */

void net_shutdown(NetCtx *ctx)
{
    close_sock(&ctx->tcp_sock);
    close_sock(&ctx->listen_sock);
    close_sock(&ctx->udp_sock);
    socExit();
    free(s_soc_buf);
    s_soc_buf = NULL;
}

/* ── net_exchange_sessions ────────────────────────────────────────── */

int net_exchange_sessions(NetCtx *ctx, PldSessionLog *local, int *new_count_out)
{
    int fd = ctx->tcp_sock;
    *new_count_out = 0;

    PldSession *remote_buf = malloc(PLD_SESSION_COUNT * sizeof(PldSession));
    if (!remote_buf) return -1;

    u32 remote_count = 0;
    u32 local_count  = (u32)local->count;

    if (ctx->role == NET_ROLE_HOST) {
        if (send_all(fd, &local_count, 4) != 4) goto fail;
        if (local_count > 0) {
            int bytes = (int)(local_count * sizeof(PldSession));
            if (send_all(fd, local->entries, bytes) != bytes) goto fail;
        }
        if (recv_all(fd, &remote_count, 4) != 4) goto fail;
        if (remote_count > PLD_SESSION_COUNT) goto fail;
        if (remote_count > 0) {
            int bytes = (int)(remote_count * sizeof(PldSession));
            if (recv_all(fd, remote_buf, bytes) != bytes) goto fail;
        }
    } else {
        if (recv_all(fd, &remote_count, 4) != 4) goto fail;
        if (remote_count > PLD_SESSION_COUNT) goto fail;
        if (remote_count > 0) {
            int bytes = (int)(remote_count * sizeof(PldSession));
            if (recv_all(fd, remote_buf, bytes) != bytes) goto fail;
        }
        if (send_all(fd, &local_count, 4) != 4) goto fail;
        if (local_count > 0) {
            int bytes = (int)(local_count * sizeof(PldSession));
            if (send_all(fd, local->entries, bytes) != bytes) goto fail;
        }
    }

    PldSessionLog remote_log = { remote_buf, (int)remote_count };
    int added = pld_merge_sessions(local, &remote_log, false);
    free(remote_buf);
    if (added < 0) return -1;
    *new_count_out = added;
    return 0;

fail:
    free(remote_buf);
    return -1;
}

/* ── net_exchange_summaries ───────────────────────────────────────── */

int net_exchange_summaries(NetCtx *ctx, PldFile *local, int *new_count_out)
{
    int fd = ctx->tcp_sock;
    *new_count_out = 0;

    PldSummary local_buf[PLD_SUMMARY_COUNT];
    u32 local_count = 0;
    for (int i = 0; i < PLD_SUMMARY_COUNT; i++) {
        if (!pld_summary_is_empty(&local->summaries[i]))
            local_buf[local_count++] = local->summaries[i];
    }

    PldSummary *remote_buf = malloc(PLD_SUMMARY_COUNT * sizeof(PldSummary));
    if (!remote_buf) return -1;

    u32 remote_count = 0;

    if (ctx->role == NET_ROLE_HOST) {
        if (send_all(fd, &local_count, 4) != 4) goto fail;
        if (local_count > 0) {
            int bytes = (int)(local_count * sizeof(PldSummary));
            if (send_all(fd, local_buf, bytes) != bytes) goto fail;
        }
        if (recv_all(fd, &remote_count, 4) != 4) goto fail;
        if (remote_count > PLD_SUMMARY_COUNT) goto fail;
        if (remote_count > 0) {
            int bytes = (int)(remote_count * sizeof(PldSummary));
            if (recv_all(fd, remote_buf, bytes) != bytes) goto fail;
        }
    } else {
        if (recv_all(fd, &remote_count, 4) != 4) goto fail;
        if (remote_count > PLD_SUMMARY_COUNT) goto fail;
        if (remote_count > 0) {
            int bytes = (int)(remote_count * sizeof(PldSummary));
            if (recv_all(fd, remote_buf, bytes) != bytes) goto fail;
        }
        if (send_all(fd, &local_count, 4) != 4) goto fail;
        if (local_count > 0) {
            int bytes = (int)(local_count * sizeof(PldSummary));
            if (send_all(fd, local_buf, bytes) != bytes) goto fail;
        }
    }

    int added = pld_merge_summaries(local, remote_buf, (int)remote_count, false);
    free(remote_buf);
    if (added < 0) return -1;
    *new_count_out = added;
    return 0;

fail:
    free(remote_buf);
    return -1;
}

/* ── net_exchange_title_names ─────────────────────────────────────── */

int net_exchange_title_names(NetCtx *ctx)
{
    int fd = ctx->tcp_sock;

    const TitleNameEntry *local;
    int local_count_i;
    title_names_get_all(&local, &local_count_i);
    u32 local_count = (u32)local_count_i;

    /* Allocate receive buffer for up to TITLE_NAMES_MAX remote entries */
    TitleNameEntry *remote_buf =
        (TitleNameEntry *)malloc(TITLE_NAMES_MAX * sizeof(TitleNameEntry));
    if (!remote_buf) return -1;

    u32 remote_count = 0;

    if (ctx->role == NET_ROLE_HOST) {
        if (send_all(fd, &local_count, 4) != 4) goto fail;
        if (local_count > 0) {
            int bytes = (int)(local_count * sizeof(TitleNameEntry));
            if (send_all(fd, local, bytes) != bytes) goto fail;
        }
        if (recv_all(fd, &remote_count, 4) != 4) goto fail;
        if (remote_count > TITLE_NAMES_MAX) goto fail;
        if (remote_count > 0) {
            int bytes = (int)(remote_count * sizeof(TitleNameEntry));
            if (recv_all(fd, remote_buf, bytes) != bytes) goto fail;
        }
    } else {
        if (recv_all(fd, &remote_count, 4) != 4) goto fail;
        if (remote_count > TITLE_NAMES_MAX) goto fail;
        if (remote_count > 0) {
            int bytes = (int)(remote_count * sizeof(TitleNameEntry));
            if (recv_all(fd, remote_buf, bytes) != bytes) goto fail;
        }
        if (send_all(fd, &local_count, 4) != 4) goto fail;
        if (local_count > 0) {
            int bytes = (int)(local_count * sizeof(TitleNameEntry));
            if (send_all(fd, local, bytes) != bytes) goto fail;
        }
    }

    title_names_merge(remote_buf, (int)remote_count);
    free(remote_buf);
    return 0;

fail:
    free(remote_buf);
    return -1;
}
