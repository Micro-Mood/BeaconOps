/**
 * @file pa_mqs.h
 * @brief 优先级感知消息队列(带防饥饿) — spec §4.1–§4.3
 *
 * 设计说明:
 *  - 容量固定 PA_MQS_CAP=16,每槽承载完整 msg_t(独占其堆指针)。
 *  - pa_mqs_admit 接收 msg_t 后将其堆所有权移入槽内,源 m 被 memset 清零。
 *  - 状态全部装入 pa_mqs_ctx_t,无单例,允许 by-value 持有于上层句柄。
 *  - 非线程安全:由调用方(msg 顶层)保证单任务串行访问。
 */

#ifndef PA_MQS_H
#define PA_MQS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PA_MQS_CAP   16

/**
 * @brief 队列上下文(可由上层 by-value 持有)
 */
typedef struct {
    msg_t  slots[PA_MQS_CAP];
    size_t count;
    bool   initialized;
} pa_mqs_ctx_t;

esp_err_t  pa_mqs_init  (pa_mqs_ctx_t *ctx);
void       pa_mqs_deinit(pa_mqs_ctx_t *ctx);

size_t     pa_mqs_count (pa_mqs_ctx_t *ctx);

/**
 * @brief 接纳一条消息;堆所有权从 m 转移入槽
 *
 * @return ESP_OK / ESP_ERR_TIMEOUT(已过期) / ESP_ERR_NO_MEM(满且无优先) /
 *         ESP_ERR_INVALID_ARG
 */
esp_err_t  pa_mqs_admit (pa_mqs_ctx_t *ctx, msg_t *m, uint32_t now_ts);

/** 当前调度分:W[level] + aging_bonus */
uint32_t   pa_mqs_score (const msg_t *m);

/** 取分最高槽;exclude_id 跳过(允许 NULL) */
const msg_t *pa_mqs_pick_best(pa_mqs_ctx_t *ctx, const char *exclude_id);

/** 只读迭代,越界返回 NULL */
const msg_t *pa_mqs_get_at  (pa_mqs_ctx_t *ctx, size_t i);

/** 按 id 移除并释放;true=成功 */
bool         pa_mqs_remove  (pa_mqs_ctx_t *ctx, const char *id);

/** 是否已有 id 对应槽 */
bool         pa_mqs_contains(pa_mqs_ctx_t *ctx, const char *id);

/**
 * @brief 老化推进:非 EMERG 槽 aging_bonus += sec_delta,封顶 3600
 *
 * 原始定义为"1Hz 驱动",但 light sleep 会压扫调度 tick — 改为上层传入
 * 实际墙钟秒数增量,唭醒后一次补齐,避免 aging_bonus 与现实不同步。
 */
void         pa_mqs_tick_aging(pa_mqs_ctx_t *ctx, uint32_t sec_delta);

/**
 * @brief 扫除 TTL 过期的非 EMERG 槽(跳过 exclude_id)
 *
 * @param cb 每条移除前回调(NULL 允许)
 * @return 移除条数
 */
typedef void (*pa_mqs_expire_cb)(const msg_t *m, void *user);
size_t pa_mqs_sweep_expired(pa_mqs_ctx_t *ctx,
                            uint32_t now_ts,
                            const char *exclude_id,
                            pa_mqs_expire_cb cb,
                            void *user);

#ifdef __cplusplus
}
#endif

#endif /* PA_MQS_H */
