/**
 * @file lru_dedup.h
 * @brief 4 字节直接映射 token 缓存,用于 MQTT msg_id 去重(spec §4.1 / §10.1)
 *
 * 设计说明:
 *  - 容量 LRU_DEDUP_SLOTS=1024 × 4B = 4 KB,常驻 RAM;可选 SPIFFS 持久。
 *    (历史值 4096/16KB 在 ESP32-C3 上启动期常因 internal SRAM 碎片化拿不到
 *     连续大块;消息量级=日均 < 100 条,1024 槽假阳性 ≈ 0.1%,工程足够。)
 *  - 直接映射:碰撞即覆盖(可接受的"假未命中")。
 *  - 状态全部装入 lru_dedup_ctx_t,无单例;ctx 由上层 by-value 持有。
 *  - 非线程安全:由调用方保证单任务串行访问。
 */

#ifndef LRU_DEDUP_H
#define LRU_DEDUP_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LRU_DEDUP_SLOTS  1024u

/**
 * @brief 去重缓存上下文
 */
typedef struct {
    uint32_t *slots;       ///< 长度 LRU_DEDUP_SLOTS;0 = 空
    char     *path;        ///< strdup 的 SPIFFS 路径;NULL = 仅 RAM
    bool      dirty;
    bool      initialized;
} lru_dedup_ctx_t;

/**
 * @brief 初始化
 *
 * @param ctx         输出
 * @param spiffs_path 持久化路径;NULL = 仅 RAM(不 load/flush)
 */
esp_err_t lru_dedup_init  (lru_dedup_ctx_t *ctx, const char *spiffs_path);

/** 释放(含 flush) */
void      lru_dedup_deinit(lru_dedup_ctx_t *ctx);

/**
 * @brief 只读探测:对应 token 是否已驻留(不修改缓存)
 *
 * 拆分自原 lru_dedup_seen_or_add — 配合 admit 使用,先 seen 决定是否丢弃,
 * admit 成功后再 lru_dedup_add,避免"admit 失败 + token 已写入"的永封。
 *
 * @return true=已存在;false=未见;空/NULL id 永远返回 false
 */
bool      lru_dedup_seen(lru_dedup_ctx_t *ctx, const char *msg_id);

/** 写入 token(已存在则 no-op);空/NULL id 安全 */
void      lru_dedup_add (lru_dedup_ctx_t *ctx, const char *msg_id);

/** 强制写盘(若 RAM only 或未脏 → no-op) */
esp_err_t lru_dedup_flush(lru_dedup_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LRU_DEDUP_H */
