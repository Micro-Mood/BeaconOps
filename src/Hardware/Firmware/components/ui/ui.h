/**
 * @file ui.h
 * @brief BeaconOps 屏幕与消息卡片栈 — 句柄化封装
 *
 * 设计说明:
 *  - ui 组件不负责 LVGL 与显示驱动初始化(由 st7789 组件 + esp_lvgl_port
 *    在 main 中先行完成);ui_init 仅在已运行的 LVGL 上构建对象树。
 *  - 所有公共 API 内部自行抢 `lvgl_port_lock`,可在任意 FreeRTOS 任务调用。
 *  - level 0..3 与 msg/notify/audio_router 共享同一域(int 中性传递)。
 *  - 字体 NULL → 使用配置默认("源黑体 16 CJK")。
 *  - UI 只渲染 msg 给出的卡片快照,不自行维护消息业务队列。
 */

#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 与 msg_level_e 数值等价(避免循环依赖,不 include msg.h) */
typedef enum {
    UI_LV_INFO   = 0,
    UI_LV_NOTICE = 1,
    UI_LV_WARN   = 2,
    UI_LV_EMERG  = 3,
} ui_level_e;

typedef enum {
    UI_WIFI_OFF = 0,
    UI_WIFI_LINKING,
    UI_WIFI_OK,
    UI_WIFI_FAIL,
} ui_wifi_state_e;

/** MQTT 状态:对应顶部云图标颜色(灰=断/绿=连/红=失败) */
typedef enum {
    UI_MQTT_OFF = 0,
    UI_MQTT_LINKING,
    UI_MQTT_OK,
    UI_MQTT_FAIL,
} ui_mqtt_state_e;


/** IMU 行为状态(仅 UI 用,与 sensor_task.behavior_state_e 数值同步) */
typedef enum {
    UI_IMU_STATIC    = 0,
    UI_IMU_WALK_SLOW = 1,
    UI_IMU_WALK_FAST = 2,
    UI_IMU_RUN       = 3,
    UI_IMU_SHAKE     = 4,
} ui_imu_state_e;

/** Toast 视觉级别(独立于卡片 level,仅 ui_toast_push 使用) */
typedef enum {
    UI_TOAST_INFO  = 0,   ///< 蓝边,自动消失
    UI_TOAST_WARN  = 1,   ///< 橙边,自动消失
    UI_TOAST_ERROR = 2,   ///< 红边,持续显示直到清除
} ui_toast_level_e;

/**
 * @brief 卡片配色(每 level 一行;NULL → 走内置默认)
 */
typedef struct {
    uint32_t bg;
    uint32_t title;
    uint32_t border;
} ui_card_theme_t;

typedef struct {
    const char *id;
    const char *title;
    const char *body;
    int         level;
} ui_card_view_t;

/**
 * @brief ui 配置
 */
typedef struct {
    /* 屏幕几何(0 → 320×172) */
    uint16_t hor_res;
    uint16_t ver_res;
    /* 字体(NULL 各自有默认) */
    const lv_font_t *font_title;
    const lv_font_t *font_body;
    const lv_font_t *font_status;
    /* 消息卡片栈(历史字段保留兼容;顺序由 msg 快照决定) */
    uint8_t  card_max;          ///< 0 → 4
    uint16_t card_anim_ms;      ///< 0 → 250
    uint8_t  card_stagger_px;   ///< 0 → 7
    /* 配色(NULL → 内置默认 4 色) */
    const ui_card_theme_t *themes;
    /* LVGL 锁等待 */
    uint32_t lock_timeout_ms;   ///< 0 → 200
    /* 闲置文(NULL → "BeaconOps  ready") */
    const char *idle_text;
} ui_config_t;

typedef struct ui_dev_s ui_dev_t;

esp_err_t ui_init  (ui_dev_t **dev, const ui_config_t *config);
esp_err_t ui_deinit(ui_dev_t **dev);

/* ---- 业务 API ---------------------------------------------------------- */

/** 兼容旧单卡入口;消息队列由 msg/pa_mqs 管理;任意线程 */
esp_err_t ui_push_card    (ui_dev_t *dev, const char *title, const char *body, int level);

/** 用 msg 调度器给出的快照刷新卡片栈;cards[0] 必须是当前可确认消息 */
esp_err_t ui_set_cards    (ui_dev_t *dev, const ui_card_view_t *cards, size_t count);

/** 弹出当前消息卡片(动画化);任意线程;栈模式下通常由 ui_set_cards 驱动 */
esp_err_t ui_dismiss_front(ui_dev_t *dev);

/** 顶部 Wi-Fi 图标颜色 */
esp_err_t ui_set_wifi     (ui_dev_t *dev, ui_wifi_state_e state);

/**
 * @brief 顶部时间 + 电量
 * @param time_str NULL 则不更新时间
 * @param bat_pct  <0 则不更新电量
 */
esp_err_t ui_set_status   (ui_dev_t *dev, const char *time_str, int bat_pct);

/**
 * @brief 充电状态 — 为 true 时电池图标绘为绿色
 */
esp_err_t ui_set_charging (ui_dev_t *dev, bool charging);

/**
 * @brief 顶部 MQTT 云图标颜色
 */
esp_err_t ui_set_mqtt     (ui_dev_t *dev, ui_mqtt_state_e state);

/**
 * @brief 顶部 IMU 活动指示
 *  - STATIC      : 灰点
 *  - WALK_SLOW/2 : 绿点(走动时跳动动画 200ms 周期)
 *  - RUN         : 黄点(快速跳动 100ms)
 *  - SHAKE       : 红点(强闪)
 */
esp_err_t ui_set_imu      (ui_dev_t *dev, ui_imu_state_e state);

/**
 * @brief Wi-Fi 信号强度,与 ui_set_wifi 配合改变 wifi 图标的格数颜色梯度
 * @param rssi  dBm,约 [-95..-30];INT_MIN 表示未知
 */
esp_err_t ui_set_rssi     (ui_dev_t *dev, int rssi);

/**
 * @brief 更新开机加载屏底部的步骤文本(主页显示后此调用静默忽略)
 *  @param step  短文本,如 "Wi-Fi connecting..."、"MQTT online"
 */
esp_err_t ui_set_boot_step(ui_dev_t *dev, const char *step);

/* ---- 主页(替换 idle) -------------------------------------------------- */

/**
 * @brief 显示/更新主页(48px 大字时间 + 日期/星期 + 姓名/工号 + 今日步数)
 *  - 调用一次后自动隐藏 idle_label
 *  - 各字段为 NULL/负数时保留旧值;首次调用时 NULL 字段显示占位
 *  @param hhmm        如 "12:34"
 *  @param date_text   如 "11月02日 周日"
 *  @param name        姓名
 *  @param work_id     工号(可为空字符串)
 *  @param steps_today 今日步数;<0 不更新
 */
esp_err_t ui_show_home    (ui_dev_t *dev,
                           const char *hhmm,
                           const char *date_text,
                           const char *name,
                           const char *work_id,
                           int         steps_today);

/* ---- Toast 弹窗(独立对象树,不进卡片栈) ------------------------------ */

/**
 * @brief 推送一条 toast。固定宽度 180px,高度按内容自适应。
 *        最多 3 条同时显示,溢出时丢弃最旧的。
 *  - INFO/WARN:auto_ms 后自动消失(0 → 默认 INFO 3000ms / WARN 5000ms)
 *  - ERROR    :忽略 auto_ms,保留直到 ui_toast_clear_errors 或摇一摇清除
 */
esp_err_t ui_toast_push   (ui_dev_t *dev,
                           ui_toast_level_e level,
                           const char *text,
                           uint32_t   auto_ms);

/** 清除所有 ERROR 级 toast(给摇一摇/手动确认调用) */
esp_err_t ui_toast_clear_errors(ui_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
