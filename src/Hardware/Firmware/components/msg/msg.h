/**
 * @file msg.h
 * @brief 消息子系统对外接口 — 类型定义 + 顶层 msg_dev_t 句柄 API
 *
 * 设计说明:
 *  - msg_t 是数据类型(parser/pa_mqs 共用),无状态。
 *  - msg_dev_t 是顶层调度器句柄,内部聚合 pa_mqs_ctx_t + lru_dedup_ctx_t,
 *    提供 ingest / on_shake 入口,通过 UI 快照 / TTS / ACK 回调与外部解耦。
 *  - 调度状态机参见 spec §4.3:1Hz 老化 + TTL 扫除 + min_display_ms 防抢占
 *    + shake → ack → pop → re-pick。
 *
 * 数值约定:msg_level_e 0..3 与 ui 层、notify 层、audio_router 层 level 等价,
 *           允许直接 cast int。
 */

#ifndef MSG_H
#define MSG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  数据类型(parser / pa_mqs / 调度器共用)
 * ============================================================ */

typedef enum {
    MSG_LEVEL_INFO   = 0,
    MSG_LEVEL_NOTICE = 1,
    MSG_LEVEL_WARN   = 2,
    MSG_LEVEL_EMERG  = 3,
} msg_level_e;

#define MSG_FLAG_HAS_DISPLAY  (1u << 0)
#define MSG_FLAG_HAS_AUDIO    (1u << 1)   /* 兼容旧字段,protocol v1 永远不设置 */

/**
 * @brief 服务器要求的 ack 节点(由 parser 写入 msg_t.ack_mode)
 *
 * 仅在 mode <= 当前事件类型时发送对应 ack:
 *   NONE         → 不发任何 ack(info/notice 默认)
 *   RECEIVED     → 仅在投递入队后发 received
 *   DISPLAYED    → 在首次上屏发 displayed(隐含发 received)
 *   ACKNOWLEDGED → 直至用户摇晃确认发 acknowledged(隐含发前两阶段)
 */
typedef enum {
    MSG_ACK_MODE_NONE         = 0,
    MSG_ACK_MODE_RECEIVED     = 1,
    MSG_ACK_MODE_DISPLAYED    = 2,
    MSG_ACK_MODE_ACKNOWLEDGED = 3,
} msg_ack_mode_e;

/**
 * @brief 单条消息(内部堆字段独占)
 */
typedef struct {
    char        id[37];          ///< 服务器分配 UUID;空字符串表示无
    uint32_t    arrive_ts;       ///< 服务器 unix 秒
    uint32_t    enqueue_ts;      ///< 本地入队时刻(SNTP 已同步则 unix,否则 boot_ms)
    uint32_t    shown_at_ms;     ///< 首次显示 boot_ms(由调度器写)
    uint32_t    expire_ts;       ///< arrive_ts + ttl;0 = 永不过期
    msg_level_e    level;
    msg_ack_mode_e ack_mode;     ///< parser 写入,调度器据此决定发哪些 ack
    uint16_t    aging_bonus;
    uint8_t     flags;
    char       *title;           ///< 独占,可为 NULL
    char       *body;            ///< 独占,可为 NULL
    char       *audio_text;      ///< 独占,protocol v1 始终 NULL(留存以兼容旧编译)
} msg_t;

/** 释放 msg 内堆字段并清零;NULL 安全 */
void msg_clear(msg_t *m);

/* ============================================================
 *  顶层 msg_dev_t 句柄
 * ============================================================ */

/**
 * @brief ack 类型(由调度器在不同时机发出)
 */
typedef enum {
    MSG_ACK_RECEIVED  = 0,   ///< 已落盘/入队,用于服务端停止下行重发
    MSG_ACK_DISPLAYED = 1,   ///< 首次渲染上屏
    MSG_ACK_ACKED     = 2,   ///< 用户摇晃确认
    MSG_ACK_EXPIRED   = 3,   ///< TTL 到期(非 EMERG)
} msg_ack_kind_e;

#define MSG_UI_STACK_MAX 4

typedef struct {
    const char *id;
    const char *title;
    const char *body;
    int         level;
} msg_ui_card_view_t;

/** 字符串生命周期仅限回调内 */
typedef void (*msg_ui_show_fn)   (const char *title, const char *body,
                                  int level, void *user);
typedef void (*msg_ui_dismiss_fn)(void *user);
typedef void (*msg_ui_stack_fn)  (const msg_ui_card_view_t *cards,
                                  size_t count, void *user);
typedef void (*msg_tts_fn)       (const char *text, int level, void *user);
typedef void (*msg_ack_fn)       (const char *msg_id, msg_ack_kind_e k, void *user);
typedef void (*msg_notify_fn)    (int level, void *user);
typedef void (*msg_confirm_fn)   (void *user);

/**
 * @brief msg 顶层配置
 */
typedef struct {
    /* UI(必填) */
    msg_ui_show_fn    ui_show;
    msg_ui_dismiss_fn ui_dismiss;
    msg_ui_stack_fn   ui_stack;       ///< 可选;存在时 UI 由完整卡片栈快照驱动
    void             *ui_user;
    /* 可选 sink */
    msg_tts_fn        on_tts;
    void             *tts_user;
    msg_ack_fn        on_ack;
    void             *ack_user;
    msg_notify_fn     on_notify;     ///< 新消息被接纳入队时触发提示音
    void             *notify_user;
    msg_confirm_fn    on_confirm;    ///< 当前消息被摇晃确认/弹出时触发确认音
    void             *confirm_user;
    /* 持久化 */
    const char       *spiffs_lru_path;     ///< NULL = LRU 仅 RAM
    const char       *spiffs_msg_prefix;   ///< NULL → "/spiffs/mq_";"" = 禁用消息落盘
    uint32_t          msg_store_max;       ///< 0 → 64
    uint32_t          lru_flush_period_s;  ///< 0 → 300
    /* 调度参数 */
    uint32_t          min_display_ms;      ///< 0 → 2000
    uint32_t          tick_period_ms;      ///< 0 → 100
    uint8_t           task_prio;           ///< 0 → tskIDLE_PRIORITY + 4
    uint16_t          task_stack;          ///< 0 → 4096
} msg_config_t;

typedef struct msg_dev_s msg_dev_t;

esp_err_t msg_init  (msg_dev_t **dev, const msg_config_t *config);
esp_err_t msg_deinit(msg_dev_t **dev);

/**
 * @brief 投喂一条新到达的 MQTT payload(safe from any task)
 *
 * @return ESP_OK / ESP_ERR_INVALID_STATE / parse 或 admit 的错误码
 */
esp_err_t msg_ingest  (msg_dev_t *dev, const char *payload, int len);

/** 标记一次摇晃事件(由 sensor 转发);ISR-like task 安全 */
void      msg_on_shake(msg_dev_t *dev);

/** 出站最终 ack 成功送出后清理 SPIFFS 消息文件(仅 ACKED/EXPIRED 会删除) */
void      msg_on_ack_delivered(msg_dev_t *dev, const char *msg_id,
                               msg_ack_kind_e kind);

/** parser 失败 + admit 拒收的累计计数 */
uint32_t  msg_get_drop_count(msg_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* MSG_H */
