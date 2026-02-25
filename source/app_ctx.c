#include "app_ctx.h"

void app_ctx_rebuild(AppCtx *ctx)
{
    ctx->n = collect_valid(&ctx->pld, ctx->valid,
                           ctx->show_system, ctx->show_unknown,
                           ctx->settings.min_play_secs, &ctx->hidden);
    if (view_is_rank(ctx->view_mode)) {
        ctx->rank_count = build_rankings(
            ctx->valid, ctx->n, ctx->view_mode,
            &ctx->sessions, &ctx->pld,
            ctx->ranked, ctx->rank_metric);
        ctx->rank_sel    = 0;
        ctx->rank_scroll = 0;
        ctx->rank_anim_frame = 0;
    } else {
        sort_valid(ctx->valid, ctx->n, ctx->view_mode,
                   &ctx->sessions, &ctx->pld);
        ctx->sel        = 0;
        ctx->scroll_top = 0;
        ctx->scroll_y   = 0.0f;
        ctx->list_anim_frame = 0;
    }
}
