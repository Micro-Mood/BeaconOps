/**
 * @file pa_mqs.c
 * @brief 优先级感知消息队列实现(ctx 化)
 */

#include "pa_mqs.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "pa_mqs";

/* w[level] = {1,2,5,100} per spec §4.2,放大 100 倍以容纳 aging_bonus<=3600 */
static const uint32_t W[] = { 100u, 200u, 500u, 10000u };

#define AGING_CAP  3600u

/* ---- 槽工具 ------------------------------------------------------------ */

static bool slot_used(pa_mqs_ctx_t *ctx, size_t i)
{
    const msg_t *s = &ctx->slots[i];
    return s->title || s->body || s->audio_text || s->id[0];
}

static int slot_find(pa_mqs_ctx_t *ctx, const char *id)
{
    if (!id || !*id) return -1;
    for (size_t i = 0; i < PA_MQS_CAP; ++i) {
        if (slot_used(ctx, i) && strcmp(ctx->slots[i].id, id) == 0) return (int)i;
    }
    return -1;
}

static int slot_first_free(pa_mqs_ctx_t *ctx)
{
    for (size_t i = 0; i < PA_MQS_CAP; ++i) {
        if (!slot_used(ctx, i)) return (int)i;
    }
    return -1;
}

static int slot_lowest(pa_mqs_ctx_t *ctx)
{
    int best = -1;
    uint32_t best_score = UINT32_MAX;
    uint32_t best_ts    = UINT32_MAX;
    for (size_t i = 0; i < PA_MQS_CAP; ++i) {
        if (!slot_used(ctx, i)) continue;
        uint32_t s = pa_mqs_score(&ctx->slots[i]);
        if (s < best_score ||
            (s == best_score && ctx->slots[i].enqueue_ts < best_ts)) {
            best       = (int)i;
            best_score = s;
            best_ts    = ctx->slots[i].enqueue_ts;
        }
    }
    return best;
}

/** 移入(zero src);若槽原本空闲则 count++ */
static void slot_move_in(pa_mqs_ctx_t *ctx, size_t i, msg_t *src)
{
    bool was_used = slot_used(ctx, i);
    msg_clear(&ctx->slots[i]);
    ctx->slots[i] = *src;
    memset(src, 0, sizeof(*src));
    if (!was_used && ctx->count < PA_MQS_CAP) ctx->count++;
}

/* ============================================================
 *  公共 API
 * ============================================================ */

esp_err_t pa_mqs_init(pa_mqs_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    if (ctx->initialized) return ESP_OK;
    memset(ctx, 0, sizeof(*ctx));
    ctx->initialized = true;
    return ESP_OK;
}

void pa_mqs_deinit(pa_mqs_ctx_t *ctx)
{
    if (!ctx || !ctx->initialized) return;
    for (size_t i = 0; i < PA_MQS_CAP; ++i) msg_clear(&ctx->slots[i]);
    ctx->count       = 0;
    ctx->initialized = false;
}

size_t pa_mqs_count(pa_mqs_ctx_t *ctx)
{
    return (ctx && ctx->initialized) ? ctx->count : 0;
}

uint32_t pa_mqs_score(const msg_t *m)
{
    if (!m) return 0;
    int lv = (int)m->level;
    if (lv < 0 || lv > 3) lv = 0;
    return W[lv] + (uint32_t)m->aging_bonus;
}

esp_err_t pa_mqs_admit(pa_mqs_ctx_t *ctx, msg_t *m, uint32_t now_ts)
{
    if (!ctx || !ctx->initialized || !m) return ESP_ERR_INVALID_ARG;
    if (m->expire_ts && m->expire_ts <= now_ts && m->level != MSG_LEVEL_EMERG) {
        return ESP_ERR_TIMEOUT;
    }

    /* 同 id 已在队列 → 替换(服务器重发/迟到更新) */
    int existing = slot_find(ctx, m->id);
    if (existing >= 0) {
        slot_move_in(ctx, (size_t)existing, m);
        return ESP_OK;
    }

    int free_idx = slot_first_free(ctx);
    if (free_idx >= 0) {
        slot_move_in(ctx, (size_t)free_idx, m);
        return ESP_OK;
    }

    /* 满。EMERG 强行驱逐最弱槽 */
    if (m->level == MSG_LEVEL_EMERG) {
        int low = slot_lowest(ctx);
        if (low < 0) return ESP_ERR_NO_MEM;
        ESP_LOGW(TAG, "EMERG 驱逐 slot %d (score=%lu)",
                 low, (unsigned long)pa_mqs_score(&ctx->slots[low]));
        slot_move_in(ctx, (size_t)low, m);
        return ESP_OK;
    }

    /* 非 EMERG:仅在严格高于最弱槽时替换(忽略 incoming aging) */
    int low = slot_lowest(ctx);
    if (low < 0) return ESP_ERR_NO_MEM;

    msg_t scratch = *m;
    scratch.aging_bonus = 0;
    if (pa_mqs_score(&scratch) > pa_mqs_score(&ctx->slots[low])) {
        slot_move_in(ctx, (size_t)low, m);
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

const msg_t *pa_mqs_pick_best(pa_mqs_ctx_t *ctx, const char *exclude_id)
{
    if (!ctx || !ctx->initialized) return NULL;
    int best = -1;
    uint32_t best_score = 0;
    uint32_t best_ts    = UINT32_MAX;
    for (size_t i = 0; i < PA_MQS_CAP; ++i) {
        if (!slot_used(ctx, i)) continue;
        if (exclude_id && *exclude_id && strcmp(ctx->slots[i].id, exclude_id) == 0) continue;
        uint32_t s = pa_mqs_score(&ctx->slots[i]);
        if (s > best_score ||
            (s == best_score && ctx->slots[i].enqueue_ts < best_ts)) {
            best       = (int)i;
            best_score = s;
            best_ts    = ctx->slots[i].enqueue_ts;
        }
    }
    return (best >= 0) ? &ctx->slots[best] : NULL;
}

const msg_t *pa_mqs_get_at(pa_mqs_ctx_t *ctx, size_t i)
{
    if (!ctx || !ctx->initialized || i >= PA_MQS_CAP || !slot_used(ctx, i)) return NULL;
    return &ctx->slots[i];
}

bool pa_mqs_remove(pa_mqs_ctx_t *ctx, const char *id)
{
    if (!ctx || !ctx->initialized) return false;
    int i = slot_find(ctx, id);
    if (i < 0) return false;
    msg_clear(&ctx->slots[i]);
    if (ctx->count) ctx->count--;
    return true;
}

bool pa_mqs_contains(pa_mqs_ctx_t *ctx, const char *id)
{
    if (!ctx || !ctx->initialized) return false;
    return slot_find(ctx, id) >= 0;
}

void pa_mqs_tick_aging(pa_mqs_ctx_t *ctx, uint32_t sec_delta)
{
    if (!ctx || !ctx->initialized || !sec_delta) return;
    for (size_t i = 0; i < PA_MQS_CAP; ++i) {
        if (!slot_used(ctx, i)) continue;
        if (ctx->slots[i].level == MSG_LEVEL_EMERG) continue;
        uint32_t b = (uint32_t)ctx->slots[i].aging_bonus + sec_delta;
        if (b > AGING_CAP) b = AGING_CAP;
        ctx->slots[i].aging_bonus = (uint16_t)b;
    }
}

size_t pa_mqs_sweep_expired(pa_mqs_ctx_t *ctx,
                            uint32_t now_ts,
                            const char *exclude_id,
                            pa_mqs_expire_cb cb,
                            void *user)
{
    if (!ctx || !ctx->initialized) return 0;
    size_t removed = 0;
    for (size_t i = 0; i < PA_MQS_CAP; ++i) {
        if (!slot_used(ctx, i)) continue;
        if (ctx->slots[i].level == MSG_LEVEL_EMERG) continue;
        if (ctx->slots[i].expire_ts == 0) continue;
        if (ctx->slots[i].expire_ts > now_ts) continue;
        if (exclude_id && *exclude_id && strcmp(ctx->slots[i].id, exclude_id) == 0) continue;

        if (cb) cb(&ctx->slots[i], user);
        msg_clear(&ctx->slots[i]);
        if (ctx->count) ctx->count--;
        removed++;
    }
    return removed;
}
