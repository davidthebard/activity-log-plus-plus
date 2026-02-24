#pragma once
#include "pld.h"
#include <3ds.h>
#include <stdbool.h>

#define NET_TCP_PORT     12345
#define NET_UDP_PORT     12346
#define NET_MAGIC        0x504C4453u   /* "PLDS" */
#define NET_SOC_BUF_SIZE 0x100000      /* 1 MiB  */

typedef enum { NET_ROLE_HOST, NET_ROLE_CLIENT } NetRole;

typedef enum {
    NET_STATE_WAITING,    /* host: broadcasting + waiting for TCP connect */
    NET_STATE_SCANNING,   /* client: waiting for UDP broadcast            */
    NET_STATE_CONNECTED,
    NET_STATE_ERROR,
} NetState;

typedef struct {
    NetRole  role;
    NetState state;
    int      tcp_sock;    /* established data socket; -1 = none  */
    int      listen_sock; /* host only: server socket; -1 = none */
    int      udp_sock;    /* discovery socket; -1 = none         */
    char     peer_ip[16]; /* dotted-decimal IP of remote peer    */
    char     own_ip[16];  /* dotted-decimal IP of this device    */
    int      bcast_timer; /* frames since last UDP broadcast     */
} NetCtx;

Result net_init(NetCtx *ctx, NetRole role);
void   net_tick(NetCtx *ctx);   /* call once per frame */
void   net_shutdown(NetCtx *ctx);

/* Transfer local sessions to peer and receive+merge peer's sessions into *local.
 * *new_count_out receives the number of unique sessions added from the peer.
 * Returns 0 on success, -1 on I/O error or buffer overflow. */
int net_exchange_sessions(NetCtx *ctx, PldSessionLog *local, int *new_count_out);

/* Transfer local summaries to peer and receive+merge peer's summaries into *local.
 * *new_count_out receives the number of new titles added from the peer.
 * Returns 0 on success, -1 on I/O error or table overflow. */
int net_exchange_summaries(NetCtx *ctx, PldFile *local, int *new_count_out);

/* Exchange title name databases with the peer; new names are merged into the
 * in-memory title_names store on both devices.
 * Returns 0 on success, -1 on I/O error. */
int net_exchange_title_names(NetCtx *ctx);
