/**
 * @file lru_dedup.c
 * @brief 4096 槽 FNV-1a 直接映射去重缓存(ctx 化)
 */

#include "lru_dedup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "lru_dedup";

#define LRU_MASK    (LRU_DEDUP_SLOTS - 1u)

/* ---- 工具 -------------------------------------------------------------- */

/** FNV-1a 32 位;0 保留为空槽 sentinel,故碰到 0 时返回 1 */
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

/** 递归创建中间目录(不含末尾文件名) */
static void mkdir_p(const char *path)
{
    if (!path) return;
    char tmp[256];
    strlcpy(tmp, path, sizeof(tmp));
    char *slash = tmp;
    if (*slash == '/') slash++;
    for (; *slash; ++slash) {
        if (*slash == '/') {
            *slash = '\0';
            (void)mkdir(tmp, 0775);
            *slash = '/';
        }
    }
}

static void load_from_disk(lru_dedup_ctx_t *ctx)
{
    if (!ctx->path) return;
    FILE *f = fopen(ctx->path, "rb");
    if (!f) {
        ESP_LOGI(TAG, "无缓存文件 %s,空启动", ctx->path);
        return;
    }
    size_t n = fread(ctx->slots, sizeof(uint32_t), LRU_DEDUP_SLOTS, f);
    fclose(f);
    if (n != LRU_DEDUP_SLOTS) {
        ESP_LOGW(TAG, "缓存文件不完整 (%u/%u),重置",
                 (unsigned)n, (unsigned)LRU_DEDUP_SLOTS);
        memset(ctx->slots, 0, LRU_DEDUP_SLOTS * sizeof(uint32_t));
    } else {
        ESP_LOGI(TAG, "缓存已加载 (%u 槽, 16KB)", (unsigned)LRU_DEDUP_SLOTS);
    }
}

/* ============================================================
 *  公共 API
 * ============================================================ */

esp_err_t lru_dedup_init(lru_dedup_ctx_t *ctx, const char *spiffs_path)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    if (ctx->initialized) return ESP_OK;
    memset(ctx, 0, sizeof(*ctx));

    /* 4KB 连续块：先尝试 PSRAM(C3 上无 PSRAM 时直接返回 NULL，无害)，
     * 再回退默认堆。1024 槽对日均 < 100 条消息的去重场景已工程足够。 */
    ctx->slots = heap_caps_calloc(LRU_DEDUP_SLOTS, sizeof(uint32_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->slots) {
        ctx->slots = heap_caps_calloc(LRU_DEDUP_SLOTS, sizeof(uint32_t),
                                      MALLOC_CAP_DEFAULT);
    }
    if (!ctx->slots) {
        ESP_LOGE(TAG, "calloc %uB 失败 (free spiram=%u, internal_largest=%u)",
                 (unsigned)(LRU_DEDUP_SLOTS * sizeof(uint32_t)),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }

    if (spiffs_path) {
        ctx->path = strdup(spiffs_path);
        if (!ctx->path) {
            free(ctx->slots);
            ctx->slots = NULL;
            return ESP_ERR_NO_MEM;
        }
        /* 父目录确保存在 */
        char dir[256];
        strlcpy(dir, ctx->path, sizeof(dir));
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            mkdir_p(dir);
            (void)mkdir(dir, 0775);
        }
        load_from_disk(ctx);
    }

    ctx->dirty       = false;
    ctx->initialized = true;
    return ESP_OK;
}

bool lru_dedup_seen(lru_dedup_ctx_t *ctx, const char *msg_id)
{
    if (!ctx || !ctx->initialized || !msg_id || !*msg_id) return false;
    uint32_t tok = fnv1a(msg_id);
    return ctx->slots[tok & LRU_MASK] == tok;
}

void lru_dedup_add(lru_dedup_ctx_t *ctx, const char *msg_id)
{
    if (!ctx || !ctx->initialized || !msg_id || !*msg_id) return;
    uint32_t tok = fnv1a(msg_id);
    uint32_t idx = tok & LRU_MASK;
    if (ctx->slots[idx] == tok) return;
    ctx->slots[idx] = tok;
    ctx->dirty = true;
}

esp_err_t lru_dedup_flush(lru_dedup_ctx_t *ctx)
{
    if (!ctx || !ctx->initialized) return ESP_ERR_INVALID_STATE;
    if (!ctx->path || !ctx->dirty) return ESP_OK;

    /* 准原子写:tmp + rename */
    char tmp[260];
    snprintf(tmp, sizeof(tmp), "%s.tmp", ctx->path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGW(TAG, "open %s 失败: %s", tmp, strerror(errno));
        return ESP_FAIL;
    }
    size_t n = fwrite(ctx->slots, sizeof(uint32_t), LRU_DEDUP_SLOTS, f);
    fclose(f);
    if (n != LRU_DEDUP_SLOTS) {
        ESP_LOGW(TAG, "写入不完整 (%u/%u)", (unsigned)n, (unsigned)LRU_DEDUP_SLOTS);
        unlink(tmp);
        return ESP_FAIL;
    }
    /* SPIFFS 的 rename 非严格原子,但 unlink+rename 是公认惯例;
     * 掉电最坏下次启动空缓存 — 可接受 */
    (void)unlink(ctx->path);
    if (rename(tmp, ctx->path) != 0) {
        ESP_LOGW(TAG, "rename 失败: %s", strerror(errno));
        return ESP_FAIL;
    }
    ctx->dirty = false;
    ESP_LOGI(TAG, "已刷盘 (16KB)");
    return ESP_OK;
}

void lru_dedup_deinit(lru_dedup_ctx_t *ctx)
{
    if (!ctx || !ctx->initialized) return;
    (void)lru_dedup_flush(ctx);
    if (ctx->slots) { free(ctx->slots); ctx->slots = NULL; }
    if (ctx->path)  { free(ctx->path);  ctx->path  = NULL; }
    ctx->dirty       = false;
    ctx->initialized = false;
}
