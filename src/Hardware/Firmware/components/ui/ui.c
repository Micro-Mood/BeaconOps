/**
 * @file ui.c
 * @brief BeaconOps 屏幕实现 — 三段式布局 + 消息卡片栈
 *
 * 布局(默认 320×172):
 *   Band 1 (top  , 20px) : time | wifi | battery        — lv_layer_top
 *   Band 2 (mid  ,100px) : 当前消息 + 后续消息卡片栈      — lv_layer_top
 *   Band 3 (bot  , 20px) : message state | shake hint    — lv_layer_top
 * 消息队列与优先级排序由 msg/pa_mqs 负责;UI 按 msg 给出的快照渲染
 * 当前项 + 后续项卡片栈,不自行推断队列顺序。
 */

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "ui";

/* 项目默认中文字体(由 generate_misans_font.py 生成) */
extern const lv_font_t lv_font_misans_bold_16_gb2312;

/* 主页大字时间使用 LVGL 内置 Montserrat 48(数字字模) */
extern const lv_font_t lv_font_montserrat_48;
/* Toast 弹窗用小字号(英文,LVGL 内置) */
extern const lv_font_t lv_font_montserrat_14;/* Loading 屏 + 主页工号中等字号 */
extern const lv_font_t lv_font_montserrat_28;
extern const lv_font_t lv_font_montserrat_20;
/* 前向声明:build_home 在 ui_init 时即调用,定义在文件下方 */
struct ui_dev_s;
static void build_home(struct ui_dev_s *d);

/* ---- 默认值 ------------------------------------------------------------ */
#define UI_DEF_W              320
#define UI_DEF_H              172
#define UI_DEF_CARD_MAX       4
#define UI_DEF_CARD_ANIM_MS   250
#define UI_DEF_CARD_STAGGER   7
#define UI_DEF_LOCK_TO_MS     200

/* 安全区(原 main.cpp 同) */ 
#define UI_PAD_X   12
#define UI_PAD_R   23
#define UI_PAD_Y   10
#define UI_BAND_H  20
#define UI_GAP     6

static const ui_card_theme_t DEFAULT_THEMES[4] = {
    /* INFO   */ {0x102030, 0x99CCFF, 0x336699},
    /* NOTICE */ {0x002850, 0x66CCFF, 0x66CCFF},
    /* WARN   */ {0x402000, 0xFFAA33, 0xFFAA33},
    /* EMERG  */ {0x500000, 0xFF6666, 0xFF5555},
};

/* ============================================================
 *  句柄
 * ============================================================ */

struct ui_dev_s {
    ui_config_t cfg;
    /* 解算后的有效配置 */
    uint16_t W, H;
    uint16_t mid_y, mid_h, bot_y, band_w;
    uint8_t  card_max;
    uint16_t card_anim_ms;
    uint8_t  card_stagger;
    uint32_t lock_to_ms;
    const lv_font_t *f_title;
    const lv_font_t *f_body;
    const lv_font_t *f_status;
    const ui_card_theme_t *themes;

    /* LVGL 对象 */
    lv_obj_t *scr_main;
    lv_obj_t *sb_root, *sb_time, *sb_imu, *sb_wifi, *sb_mqtt, *sb_bat;
    lv_obj_t *bot_root, *bot_unread, *bot_hint;
    lv_obj_t *idle_label;       /* 开机加载屏根容器(重用原名以减少外部 hide/show 逻辑修改) */
    lv_obj_t *idle_step;        /* 底部小字步骤提示 */

    /* (原点动画 timer 已被 spinner widget 取代,保留字段免外部依赖) */
    lv_timer_t      *idle_timer;

    /* 主页对象树（可见时遗住 idle_label） */
    lv_obj_t *home_root;
    lv_obj_t *home_time, *home_date, *home_name, *home_workid, *home_steps;

    /* Toast 容器(独立于卡片栈;最多 UI_TOAST_MAX 条) */
#define UI_TOAST_MAX 3
    struct {
        lv_obj_t        *root;
        lv_timer_t      *timer;     /* NULL = 永久(ERROR) */
        ui_toast_level_e level;
    } toasts[UI_TOAST_MAX];
    int toast_count;

    /* IMU 动画 */
    lv_timer_t      *imu_timer;
    ui_imu_state_e   imu_state;
    int              imu_blink_phase;

    lv_obj_t **card_stack;
    char     (*card_ids)[37];
    int        card_count;

    /* 充电状态(true → 电池图标绘为绿色) */
    bool       charging;
    int        last_bat_pct;   /* 缓存最近一次电量,-1=未知 */

    /* 最近一次 RSSI(dBm),用于 wifi 图标格数梯度 */
    int        last_rssi;

    /* MQTT 状态(影响云图标颜色) */
    ui_mqtt_state_e mqtt_state;

    bool initialized;
};

/* ---- 锁助手 ------------------------------------------------------------ */

static bool ui_lock(ui_dev_t *d)
{
    /* esp_lvgl_port 的 lock 参数是 ms;0=portMAX_DELAY */
    return lvgl_port_lock(d->lock_to_ms);
}
static void ui_unlock(ui_dev_t *d) { (void)d; lvgl_port_unlock(); }

/* ---- LVGL 小工具 ------------------------------------------------------- */

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void anim_x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }
static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

static void anim_to(lv_obj_t *obj, int32_t from, int32_t to, uint32_t ms,
                    lv_anim_path_cb_t path,
                    void (*ready_cb)(lv_anim_t *))
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_path_cb(&a, path);
    if (ready_cb) lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_start(&a);
}

static int card_target_x(ui_dev_t *d, int i) { return UI_PAD_X + i * d->card_stagger; }
static int card_target_y(ui_dev_t *d, int i) { return d->mid_y  + i * d->card_stagger; }

/* ---- 各 band 构造 ------------------------------------------------------- */

static void build_status_bar(ui_dev_t *d)
{
    d->sb_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(d->sb_root, d->band_w, UI_BAND_H);
    lv_obj_set_pos (d->sb_root, UI_PAD_X, UI_PAD_Y);
    lv_obj_set_style_bg_opa      (d->sb_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(d->sb_root, 0, 0);
    lv_obj_set_style_pad_all     (d->sb_root, 0, 0);
    lv_obj_clear_flag(d->sb_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d->sb_root, LV_OBJ_FLAG_CLICKABLE);

    d->sb_time = make_label(d->sb_root, "00:00", d->f_status, 0xFFFFFF);
    lv_obj_align(d->sb_time, LV_ALIGN_LEFT_MID, 0, 0);

    /* IMU 活动小圆点(以小符号表示);初始 STATIC = 灰 */
    d->sb_imu = make_label(d->sb_root, LV_SYMBOL_PLAY, d->f_status, 0x666666);
    lv_obj_update_layout(d->sb_time);
    lv_obj_align_to(d->sb_imu, d->sb_time, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    d->sb_bat = make_label(d->sb_root, LV_SYMBOL_BATTERY_FULL " 100%", d->f_status, 0xFFFFFF);
    lv_obj_align(d->sb_bat, LV_ALIGN_RIGHT_MID, 0, 0);

    /* MQTT 云图标(用 UPLOAD 形似云上传箭头);贴电池左侧 */
    d->sb_mqtt = make_label(d->sb_root, LV_SYMBOL_UPLOAD, d->f_status, 0x666666);
    lv_obj_update_layout(d->sb_bat);
    lv_obj_align_to(d->sb_mqtt, d->sb_bat, LV_ALIGN_OUT_LEFT_MID, -8, 0);

    d->sb_wifi = make_label(d->sb_root, LV_SYMBOL_WIFI, d->f_status, 0x666666);
    /* 始终贴在 MQTT 图标左侧,避免电量变宽时重叠 */
    lv_obj_update_layout(d->sb_mqtt);
    lv_obj_align_to(d->sb_wifi, d->sb_mqtt, LV_ALIGN_OUT_LEFT_MID, -8, 0);
}

static void build_bottom_bar(ui_dev_t *d)
{
    d->bot_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(d->bot_root, d->band_w, UI_BAND_H);
    lv_obj_set_pos (d->bot_root, UI_PAD_X, d->bot_y);
    lv_obj_set_style_bg_opa      (d->bot_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(d->bot_root, 0, 0);
    lv_obj_set_style_pad_all     (d->bot_root, 0, 0);
    lv_obj_clear_flag(d->bot_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d->bot_root, LV_OBJ_FLAG_CLICKABLE);

    d->bot_unread = make_label(d->bot_root, "No messages", d->f_status, 0x666666);
    lv_obj_align(d->bot_unread, LV_ALIGN_LEFT_MID, 0, 0);

    d->bot_hint = make_label(d->bot_root,
                             LV_SYMBOL_REFRESH " shake to confirm",
                             d->f_status, 0xFFCC00);
    lv_obj_align(d->bot_hint, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(d->bot_hint, LV_OBJ_FLAG_HIDDEN);
}

/* Loading 屏—— 标题 + spinner + 步骤文本 */
static void build_idle(ui_dev_t *d, lv_obj_t *parent)
{
    /* 根容器:填满中间区,透明背景,垂直居中堆叠 */
    d->idle_label = lv_obj_create(parent);
    lv_obj_remove_style_all(d->idle_label);
    lv_obj_set_size(d->idle_label, d->band_w, d->mid_h);
    lv_obj_set_pos (d->idle_label, UI_PAD_X, d->mid_y);
    lv_obj_set_flex_flow(d->idle_label, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(d->idle_label,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(d->idle_label, 6, 0);
    lv_obj_clear_flag(d->idle_label, LV_OBJ_FLAG_SCROLLABLE);

    /* 品牌名 —— 获取黑体,中等字号 */
    lv_obj_t *brand = lv_label_create(d->idle_label);
    lv_label_set_text(brand, "BeaconOps");
    lv_obj_set_style_text_font(brand, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(brand, lv_color_hex(0x00FF88), 0);

    /* 步骤提示 —— 小字,默认 "Booting..." */
    d->idle_step = lv_label_create(d->idle_label);
    lv_label_set_text(d->idle_step, "Booting...");
    lv_obj_set_style_text_font(d->idle_step, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(d->idle_step, lv_color_hex(0xAAAAAA), 0);
}

/* ---- 卡片工厂 ---------------------------------------------------------- */

static lv_obj_t *make_card(ui_dev_t *d, const char *title, const char *body, int level)
{
    if (level < 0 || level > 3) level = 0;
    const ui_card_theme_t *t = &d->themes[level];

    lv_obj_t *card = lv_obj_create(lv_layer_top());
    lv_obj_set_size(card, d->band_w, d->mid_h);
    lv_obj_set_style_bg_color    (card, lv_color_hex(t->bg),     0);
    lv_obj_set_style_bg_opa      (card, LV_OPA_COVER,            0);
    lv_obj_set_style_border_color(card, lv_color_hex(t->border), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius      (card, 0, 0);
    lv_obj_set_style_pad_all     (card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t_lbl = make_label(card, title ? title : "", d->f_title, t->title);
    lv_obj_align(t_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *b_lbl = make_label(card, body ? body : "", d->f_body, 0xFFFFFF);
    lv_label_set_long_mode(b_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(b_lbl, d->band_w - 16);
    lv_obj_align(b_lbl, LV_ALIGN_TOP_LEFT, 0, 22);

    return card;
}

/* ---- 当前卡片管理 ------------------------------------------------------ */

static void ui_restack(ui_dev_t *d)
{
    for (int i = d->card_count - 1; i >= 0; i--) {
        if (!d->card_stack[i]) continue;
        lv_obj_move_foreground(d->card_stack[i]);
        lv_obj_set_y(d->card_stack[i], card_target_y(d, i));
        anim_to(d->card_stack[i],
                lv_obj_get_x(d->card_stack[i]),
                card_target_x(d, i),
                d->card_anim_ms, lv_anim_path_ease_out, NULL);
    }
    lv_obj_move_foreground(d->sb_root);
    lv_obj_move_foreground(d->bot_root);
}

static void ui_update_bottom(ui_dev_t *d)
{
    if (d->card_count > 0) {
        if (d->card_count == 1) {
            lv_label_set_text(d->bot_unread, "Message");
        } else {
            char buf[24];
            snprintf(buf, sizeof(buf), "Messages %d", d->card_count);
            lv_label_set_text(d->bot_unread, buf);
        }
        lv_obj_set_style_text_color(d->bot_unread, lv_color_hex(0xFFFFFF), 0);
        lv_obj_clear_flag(d->bot_hint, LV_OBJ_FLAG_HIDDEN);
        if (d->idle_label) lv_obj_add_flag(d->idle_label, LV_OBJ_FLAG_HIDDEN);
        if (d->home_root)  lv_obj_add_flag(d->home_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(d->bot_unread, "No messages");
        lv_obj_set_style_text_color(d->bot_unread, lv_color_hex(0x666666), 0);
        lv_obj_add_flag(d->bot_hint, LV_OBJ_FLAG_HIDDEN);
        if (d->home_root) {
            /* 主页存在时优先展示主页,否则回退 idle */
            lv_obj_clear_flag(d->home_root, LV_OBJ_FLAG_HIDDEN);
            if (d->idle_label) lv_obj_add_flag(d->idle_label, LV_OBJ_FLAG_HIDDEN);
        } else if (d->idle_label) {
            lv_obj_clear_flag(d->idle_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* dismiss 动画完成回调 — 仅销毁 lv_obj;card_stack 的摘除已由 ui_dismiss_front
 * 同步完成,避免"动画未完 → 后续 push/dismiss 跳动 stack[0] → cb 误删新卡"。 */
static void front_dismiss_done(lv_anim_t *a)
{
    lv_obj_t *gone = (lv_obj_t *)a->var;
    if (gone) lv_obj_del(gone);
}

/* ============================================================
 *  init / deinit
 * ============================================================ */

esp_err_t ui_init(ui_dev_t **dev, const ui_config_t *config)
{
    if (!dev || !config) return ESP_ERR_INVALID_ARG;

    ui_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;
    d->cfg = *config;

    d->W            = config->hor_res ? config->hor_res : UI_DEF_W;
    d->H            = config->ver_res ? config->ver_res : UI_DEF_H;
    d->card_max     = config->card_max ? config->card_max : UI_DEF_CARD_MAX;
    d->card_anim_ms = config->card_anim_ms ? config->card_anim_ms : UI_DEF_CARD_ANIM_MS;
    d->card_stagger = config->card_stagger_px ? config->card_stagger_px : UI_DEF_CARD_STAGGER;
    d->lock_to_ms   = config->lock_timeout_ms ? config->lock_timeout_ms : UI_DEF_LOCK_TO_MS;
    d->f_title      = config->font_title  ? config->font_title  : &lv_font_misans_bold_16_gb2312;
    d->f_body       = config->font_body   ? config->font_body   : &lv_font_misans_bold_16_gb2312;
    d->f_status     = config->font_status ? config->font_status : &lv_font_misans_bold_16_gb2312;
    d->themes       = config->themes ? config->themes : DEFAULT_THEMES;
    d->last_bat_pct = -1;
    d->last_rssi    = INT_MIN;
    d->mqtt_state   = UI_MQTT_OFF;
    d->imu_state    = UI_IMU_STATIC;

    /* 派生几何 */
    d->mid_y  = UI_PAD_Y + UI_BAND_H + UI_GAP;
    d->bot_y  = d->H - UI_PAD_Y - UI_BAND_H;
    d->mid_h  = d->H - 2 * UI_PAD_Y - 2 * UI_BAND_H - 2 * UI_GAP;
    d->band_w = d->W - UI_PAD_X - UI_PAD_R;

    d->card_stack = calloc(d->card_max, sizeof(lv_obj_t *));
    if (!d->card_stack) { free(d); return ESP_ERR_NO_MEM; }
    d->card_ids = calloc(d->card_max, sizeof(*d->card_ids));
    if (!d->card_ids) { free(d->card_stack); free(d); return ESP_ERR_NO_MEM; }

    if (!ui_lock(d)) {
        ESP_LOGE(TAG, "lvgl_port_lock 失败");
        free(d->card_ids);
        free(d->card_stack);
        free(d);
        return ESP_ERR_TIMEOUT;
    }

    d->scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color    (d->scr_main, lv_color_black(), 0);
    lv_obj_set_style_pad_all     (d->scr_main, 0, 0);
    lv_obj_set_style_border_width(d->scr_main, 0, 0);
    lv_obj_clear_flag(d->scr_main, LV_OBJ_FLAG_SCROLLABLE);

    build_idle      (d, d->scr_main);
    build_status_bar(d);
    build_bottom_bar(d);
    /* 注：home_root 延迟到 ui_show_home 首次调用才创建。
     * 开机阶段先显示 idle_label（文本在 ui_config_t::idle_text
     * 默认 "Loading..."），等 main 完成初始化后才切换到主页。*/

    lv_scr_load(d->scr_main);
    ui_unlock(d);

    d->initialized = true;
    *dev = d;
    ESP_LOGI(TAG, "ui 初始化完成 (%ux%u, card_max=%u)",
             d->W, d->H, d->card_max);

    /* 初始 wifi 状态:linking */
    (void)ui_set_wifi(d, UI_WIFI_LINKING);
    return ESP_OK;
}

esp_err_t ui_deinit(ui_dev_t **dev)
{
    if (!dev || !*dev) return ESP_ERR_INVALID_ARG;
    ui_dev_t *d = *dev;
    if (!d->initialized) return ESP_ERR_INVALID_STATE;

    if (ui_lock(d)) {
        if (d->idle_timer) { lv_timer_del(d->idle_timer); d->idle_timer = NULL; }
        if (d->imu_timer)  { lv_timer_del(d->imu_timer);  d->imu_timer  = NULL; }
        for (int i = 0; i < d->toast_count; ++i) {
            if (d->toasts[i].timer) lv_timer_del(d->toasts[i].timer);
            if (d->toasts[i].root)  lv_obj_del(d->toasts[i].root);
        }
        d->toast_count = 0;
        if (d->scr_main) lv_obj_del(d->scr_main);
        if (d->sb_root)  lv_obj_del(d->sb_root);
        if (d->bot_root) lv_obj_del(d->bot_root);
        for (int i = 0; i < d->card_max; ++i) {
            if (d->card_stack[i]) lv_obj_del(d->card_stack[i]);
        }
        ui_unlock(d);
    }
    if (d->card_stack) free(d->card_stack);
    if (d->card_ids)   free(d->card_ids);
    d->initialized = false;
    free(d);
    *dev = NULL;
    ESP_LOGI(TAG, "ui 反初始化完成");
    return ESP_OK;
}

/* ============================================================
 *  公共业务 API
 * ============================================================ */

esp_err_t ui_push_card(ui_dev_t *dev, const char *title, const char *body, int level)
{
    ui_card_view_t card = {
        .id = "",
        .title = title,
        .body = body,
        .level = level,
    };
    return ui_set_cards(dev, &card, 1);
}

esp_err_t ui_set_cards(ui_dev_t *dev, const ui_card_view_t *cards, size_t count)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    if (count > 0 && !cards) return ESP_ERR_INVALID_ARG;
    if (count > dev->card_max) count = dev->card_max;
    if (!ui_lock(dev)) return ESP_ERR_TIMEOUT;

    for (int i = 0; i < dev->card_max; i++) {
        if ((size_t)i >= count) {
            if (dev->card_stack[i]) {
                lv_anim_delete(dev->card_stack[i], anim_x_cb);
                lv_obj_del(dev->card_stack[i]);
                dev->card_stack[i] = NULL;
            }
            dev->card_ids[i][0] = '\0';
            continue;
        }

        const char *id = cards[i].id ? cards[i].id : "";
        bool same_card = id[0] && dev->card_stack[i] && strcmp(dev->card_ids[i], id) == 0;
        if (same_card) continue;

        if (dev->card_stack[i]) {
            lv_anim_delete(dev->card_stack[i], anim_x_cb);
            lv_obj_del(dev->card_stack[i]);
        }
        dev->card_stack[i] = make_card(dev, cards[i].title, cards[i].body, cards[i].level);
        strlcpy(dev->card_ids[i], id, sizeof(dev->card_ids[i]));
        lv_obj_set_pos(dev->card_stack[i], card_target_x(dev, i), card_target_y(dev, i));
    }

    dev->card_count = (int)count;
    ui_restack(dev);
    ui_update_bottom(dev);
    ui_unlock(dev);
    return ESP_OK;
}

esp_err_t ui_dismiss_front(ui_dev_t *dev)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    if (!ui_lock(dev)) return ESP_ERR_TIMEOUT;
    if (dev->card_count <= 0 || !dev->card_stack[0]) { ui_unlock(dev); return ESP_OK; }

    lv_obj_t *front = dev->card_stack[0];
    lv_anim_delete(front, anim_x_cb);
    dev->card_stack[0] = NULL;
    dev->card_ids[0][0] = '\0';
    for (int i = 1; i < dev->card_max; ++i) {
        if (dev->card_stack[i]) {
            lv_anim_delete(dev->card_stack[i], anim_x_cb);
            lv_obj_del(dev->card_stack[i]);
            dev->card_stack[i] = NULL;
        }
        dev->card_ids[i][0] = '\0';
    }
    dev->card_count = 0;
    ui_update_bottom(dev);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, front);
    lv_anim_set_values(&a, lv_obj_get_x(front), -(int32_t)dev->W);
    lv_anim_set_time(&a, dev->card_anim_ms);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, front_dismiss_done);
    a.user_data = dev;
    lv_anim_start(&a);

    ui_unlock(dev);
    return ESP_OK;
}

esp_err_t ui_set_wifi(ui_dev_t *dev, ui_wifi_state_e state)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    static const uint32_t color[] = {
        [UI_WIFI_OFF]     = 0x444444,
        [UI_WIFI_LINKING] = 0xFFCC00,
        [UI_WIFI_OK]      = 0x00FF88,
        [UI_WIFI_FAIL]    = 0xFF4444,
    };
    if (state > UI_WIFI_FAIL) state = UI_WIFI_OFF;
    if (!ui_lock(dev)) return ESP_ERR_TIMEOUT;
    if (dev->sb_wifi) lv_obj_set_style_text_color(dev->sb_wifi, lv_color_hex(color[state]), 0);
    ui_unlock(dev);
    return ESP_OK;
}

esp_err_t ui_set_status(ui_dev_t *dev, const char *time_str, int bat_pct)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    if (!ui_lock(dev)) return ESP_ERR_TIMEOUT;
    if (time_str && dev->sb_time) lv_label_set_text(dev->sb_time, time_str);
    if (bat_pct >= 0 && dev->sb_bat) {
        if (bat_pct > 100) bat_pct = 100;
        char b[16];
        snprintf(b, sizeof(b), "%s %d%%",
                 bat_pct > 80 ? LV_SYMBOL_BATTERY_FULL :
                 bat_pct > 60 ? LV_SYMBOL_BATTERY_3    :
                 bat_pct > 40 ? LV_SYMBOL_BATTERY_2    :
                 bat_pct > 20 ? LV_SYMBOL_BATTERY_1    :
                                LV_SYMBOL_BATTERY_EMPTY,
                 bat_pct);
        lv_label_set_text(dev->sb_bat, b);
        dev->last_bat_pct = bat_pct;
        /* 重新布局 mqtt + wifi 链:贴在电量标签左侧 */
        if (dev->sb_mqtt) {
            lv_obj_update_layout(dev->sb_bat);
            lv_obj_align_to(dev->sb_mqtt, dev->sb_bat, LV_ALIGN_OUT_LEFT_MID, -8, 0);
        }
        if (dev->sb_wifi && dev->sb_mqtt) {
            lv_obj_update_layout(dev->sb_mqtt);
            lv_obj_align_to(dev->sb_wifi, dev->sb_mqtt, LV_ALIGN_OUT_LEFT_MID, -8, 0);
        }
        /* 颜色:充电中→绿;否则从电量起色 */
        uint32_t color;
        if (dev->charging)       color = 0x00FF88;
        else if (bat_pct <= 10)  color = 0xFF4444;
        else if (bat_pct <= 25)  color = 0xFFCC00;
        else                     color = 0xFFFFFF;
        lv_obj_set_style_text_color(dev->sb_bat, lv_color_hex(color),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    ui_unlock(dev);
    return ESP_OK;
}

esp_err_t ui_set_charging(ui_dev_t *dev, bool charging)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    if (!ui_lock(dev)) return ESP_ERR_TIMEOUT;
    bool changed = (dev->charging != charging);
    dev->charging = charging;
    if (changed && dev->sb_bat) {
        uint32_t color;
        int p = dev->last_bat_pct;
        if (charging)            color = 0x00FF88;
        else if (p < 0)          color = 0xFFFFFF;
        else if (p <= 10)        color = 0xFF4444;
        else if (p <= 25)        color = 0xFFCC00;
        else                     color = 0xFFFFFF;
        lv_obj_set_style_text_color(dev->sb_bat, lv_color_hex(color),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_invalidate(dev->sb_bat);
    }
    ui_unlock(dev);
    return ESP_OK;
}

/* ============================================================
 *  顶部 MQTT 云图标
 * ============================================================ */

esp_err_t ui_set_mqtt(ui_dev_t *dev, ui_mqtt_state_e state)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    static const uint32_t color[] = {
        [UI_MQTT_OFF]     = 0x444444,
        [UI_MQTT_LINKING] = 0xFFCC00,
        [UI_MQTT_OK]      = 0x00FF88,
        [UI_MQTT_FAIL]    = 0xFF4444,
    };
    if (state > UI_MQTT_FAIL) state = UI_MQTT_OFF;
    if (!ui_lock(dev)) return ESP_ERR_TIMEOUT;
    dev->mqtt_state = state;
    if (dev->sb_mqtt) {
        lv_obj_set_style_text_color(dev->sb_mqtt, lv_color_hex(color[state]),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_invalidate(dev->sb_mqtt);
    }
    ui_unlock(dev);
    return ESP_OK;
}

/* ============================================================
 *  IMU 活动指示(走动时点跳动)
 * ============================================================ */

/** 定时器回调:根据 imu_state 切换点的可见 / 颜色,模拟跳动 */
static void imu_blink_timer_cb(lv_timer_t *t)
{
    ui_dev_t *d = (ui_dev_t *)lv_timer_get_user_data(t);
    if (!d || !d->sb_imu) return;
    d->imu_blink_phase = !d->imu_blink_phase;
    uint32_t base, dim;
    switch (d->imu_state) {
        case UI_IMU_WALK_SLOW: base = 0x66FFAA; dim = 0x224433; break;
        case UI_IMU_WALK_FAST: base = 0x00FF88; dim = 0x114433; break;
        case UI_IMU_RUN:       base = 0xFFCC00; dim = 0x554400; break;
        case UI_IMU_SHAKE:     base = 0xFF4444; dim = 0x551111; break;
        default:               base = 0x666666; dim = 0x666666; break;
    }
    uint32_t c = d->imu_blink_phase ? base : dim;
    lv_obj_set_style_text_color(d->sb_imu, lv_color_hex(c),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
}

esp_err_t ui_set_imu(ui_dev_t *dev, ui_imu_state_e state)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    if (state > UI_IMU_SHAKE) state = UI_IMU_STATIC;
    if (!ui_lock(dev)) return ESP_ERR_TIMEOUT;
    dev->imu_state = state;

    /* STATIC 时停止动画,直接灰色固定 */
    if (state == UI_IMU_STATIC) {
        if (dev->imu_timer) { lv_timer_del(dev->imu_timer); dev->imu_timer = NULL; }
        if (dev->sb_imu) {
            lv_obj_set_style_text_color(dev->sb_imu, lv_color_hex(0x666666),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    } else {
        uint32_t period =
            (state == UI_IMU_RUN || state == UI_IMU_SHAKE) ? 100 : 250;
        if (!dev->imu_timer) {
            dev->imu_timer = lv_timer_create(imu_blink_timer_cb, period, dev);
        } else {
            lv_timer_set_period(dev->imu_timer, period);
        }
    }
    ui_unlock(dev);
    return ESP_OK;
}

/* ============================================================
 *  RSSI(影响 wifi 图标颜色梯度)
 * ============================================================ */

esp_err_t ui_set_rssi(ui_dev_t *dev, int rssi)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    if (!ui_lock(dev)) return ESP_ERR_TIMEOUT;
    dev->last_rssi = rssi;
    if (dev->sb_wifi && rssi != INT_MIN) {
        /* 仅当 wifi 当前为 OK 状态时才参考 RSSI 调亮度 — 否则保留原色 */
        uint32_t c;
        if      (rssi >= -55) c = 0x00FF88;   /* 4 格 */
        else if (rssi >= -65) c = 0x66CCAA;   /* 3 格 */
        else if (rssi >= -75) c = 0xFFCC00;   /* 2 格 */
        else                  c = 0xFF8844;   /* 1 格,弱 */
        lv_obj_set_style_text_color(dev->sb_wifi, lv_color_hex(c),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_invalidate(dev->sb_wifi);
    }
    ui_unlock(dev);
    return ESP_OK;
}

esp_err_t ui_set_boot_step(ui_dev_t *dev, const char *step)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    if (!ui_lock(dev)) return ESP_ERR_TIMEOUT;
    /* 主页一旦显示就静默忽略后续 boot step */
    if (dev->idle_step &&
        dev->idle_label &&
        !lv_obj_has_flag(dev->idle_label, LV_OBJ_FLAG_HIDDEN)) {
        lv_label_set_text(dev->idle_step, step ? step : "");
    }
    ui_unlock(dev);
    return ESP_OK;
}

/* ============================================================
 *  主页(替换 idle_label)
 * ============================================================ */

static void build_home(ui_dev_t *d)
{
    /* 内容区 ≈ band_w(293) × mid_h(100)。
     * 4 行垂直堆(48+16+16+16=96 + 间距)放不下 → 改为两列布局:
     *
     *   ┌───────────────────────┬─────────────────┐
     *   │       12:34 (48px)    │  Name           │
     *   │                       │  #1001          │
     *   │       Wed 05/14       │  Steps  1234    │
     *   └───────────────────────┴─────────────────┘
     *
     * 左列(0..LEFT_W):大字时间居中 + 下方日期
     * 右列(LEFT_W..band_w):姓名 / 工号 / 步数,垂直均布
     */
    d->home_root = lv_obj_create(d->scr_main);
    lv_obj_set_size(d->home_root, d->band_w, d->mid_h);
    lv_obj_set_pos (d->home_root, UI_PAD_X, d->mid_y);
    lv_obj_set_style_bg_opa      (d->home_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(d->home_root, 0, 0);
    lv_obj_set_style_pad_all     (d->home_root, 0, 0);
    lv_obj_clear_flag(d->home_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d->home_root, LV_OBJ_FLAG_CLICKABLE);

    const int LEFT_W  = 160;
    const int RIGHT_X = LEFT_W + 6;
    const int RIGHT_W = d->band_w - RIGHT_X;

    /* ---- 左列: flex 列容器, 时间+日期自动垂直居中 ---- */
    lv_obj_t *lcol = lv_obj_create(d->home_root);
    lv_obj_set_size(lcol, LEFT_W, d->mid_h);
    lv_obj_set_pos (lcol, 0, 0);
    lv_obj_set_style_bg_opa      (lcol, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lcol, 0, 0);
    lv_obj_set_style_pad_all     (lcol, 0, 0);
    lv_obj_clear_flag(lcol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(lcol, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(lcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lcol, LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(lcol, 2, 0);

    d->home_time = lv_label_create(lcol);
    lv_label_set_text(d->home_time, "--:--");
    lv_obj_set_style_text_font(d->home_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(d->home_time, lv_color_hex(0xFFFFFF), 0);

    d->home_date = lv_label_create(lcol);
    lv_label_set_text(d->home_date, "----");
    lv_obj_set_style_text_font(d->home_date, d->f_status, 0);
    lv_obj_set_style_text_color(d->home_date, lv_color_hex(0xAAAAAA), 0);

    /* ---- 右列: flex 列容器, 3 行垂直居中 ---- */
    lv_obj_t *rcol = lv_obj_create(d->home_root);
    lv_obj_set_size(rcol, RIGHT_W, d->mid_h);
    lv_obj_set_pos (rcol, RIGHT_X, 0);
    lv_obj_set_style_bg_opa      (rcol, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rcol, 0, 0);
    lv_obj_set_style_pad_all     (rcol, 0, 0);
    lv_obj_clear_flag(rcol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rcol, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(rcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rcol, LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_START,
                                LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(rcol, 6, 0);

    d->home_name = lv_label_create(rcol);
    lv_label_set_text(d->home_name, "--");
    lv_obj_set_style_text_font(d->home_name, d->f_body, 0);
    lv_obj_set_style_text_color(d->home_name, lv_color_hex(0x99CCFF), 0);
    lv_label_set_long_mode(d->home_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(d->home_name, RIGHT_W);

    d->home_workid = lv_label_create(rcol);
    lv_label_set_text(d->home_workid, "");
    lv_obj_set_style_text_font(d->home_workid, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(d->home_workid, lv_color_hex(0x888888), 0);
    lv_obj_set_width(d->home_workid, RIGHT_W);

    d->home_steps = lv_label_create(rcol);
    lv_label_set_text(d->home_steps, "Steps  0");
    lv_obj_set_style_text_font(d->home_steps, d->f_status, 0);
    lv_obj_set_style_text_color(d->home_steps, lv_color_hex(0x00FF88), 0);
    lv_obj_set_width(d->home_steps, RIGHT_W);

    lv_obj_add_flag(d->home_root, LV_OBJ_FLAG_HIDDEN);   /* 默认隐藏 */
}

esp_err_t ui_show_home(ui_dev_t *dev,
                       const char *hhmm,
                       const char *date_text,
                       const char *name,
                       const char *work_id,
                       int         steps_today)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    if (!ui_lock(dev)) return ESP_ERR_TIMEOUT;
    if (!dev->home_root) build_home(dev);

    if (hhmm && dev->home_time) lv_label_set_text(dev->home_time, hhmm);
    if (date_text && dev->home_date) lv_label_set_text(dev->home_date, date_text);
    if (name && dev->home_name)   lv_label_set_text(dev->home_name, name);
    if (work_id && dev->home_workid) {
        if (work_id[0]) {
            lv_label_set_text_fmt(dev->home_workid, "#%s", work_id);
        } else {
            lv_label_set_text(dev->home_workid, "");
        }
    }
    if (steps_today >= 0 && dev->home_steps) {
        lv_label_set_text_fmt(dev->home_steps, "Steps  %d", steps_today);
    }

    /* 隐藏 idle_label(并停止点动画),显示主页(若卡片栈为空) */
    if (dev->idle_label) lv_obj_add_flag(dev->idle_label, LV_OBJ_FLAG_HIDDEN);
    if (dev->idle_timer) { lv_timer_del(dev->idle_timer); dev->idle_timer = NULL; }
    if (dev->card_count == 0) {
        lv_obj_clear_flag(dev->home_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(dev->home_root, LV_OBJ_FLAG_HIDDEN);
    }
    ui_unlock(dev);
    return ESP_OK;
}

/* ============================================================
 *  Toast 弹窗(独立对象树,挂 lv_layer_top)
 * ============================================================ */

#define UI_TOAST_W       155
#define UI_TOAST_PAD     6
#define UI_TOAST_GAP     4
#define UI_TOAST_X_OFF   8           /* 距右边距 */

static void toast_remove_at(ui_dev_t *d, int idx);

/** Toast 计时到期回调 */
static void toast_timer_cb(lv_timer_t *t)
{
    ui_dev_t *d = (ui_dev_t *)lv_timer_get_user_data(t);
    if (!d) return;
    /* 找到对应 toast(以 timer 匹配) */
    for (int i = 0; i < d->toast_count; ++i) {
        if (d->toasts[i].timer == t) {
            toast_remove_at(d, i);
            return;
        }
    }
}

/** 重新布局所有 toast — 顶部对齐(贴在状态栏下方),从上到下堆叠
 *
 * @param skip_anim_idx 跳过该索引的 X 动画(刚推入的项负责自己的滑入动画);
 *                      传 -1 表示所有项都跰蹴到目标位 */
static void toast_relayout_ex(ui_dev_t *d, int skip_anim_idx)
{
    int y = UI_PAD_Y + UI_BAND_H + UI_GAP;
    int target_x = d->W - UI_TOAST_W - UI_TOAST_X_OFF;
    for (int i = 0; i < d->toast_count; ++i) {
        lv_obj_t *r = d->toasts[i].root;
        lv_obj_update_layout(r);
        int h = lv_obj_get_height(r);
        if (h <= 0) h = 28;

        /* Y 轴:首次显示直接 snap;否则取消旧 Y 动画后用新动画过渡 */
        int cur_y = lv_obj_get_y(r);
        if (cur_y != y) {
            lv_anim_delete(r, anim_y_cb);
            if (i == skip_anim_idx) {
                /* 新推入项还没出现过,直接 snap 到位 */
                lv_obj_set_y(r, y);
            } else {
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, r);
                lv_anim_set_values(&a, cur_y, y);
                lv_anim_set_time(&a, 180);
                lv_anim_set_exec_cb(&a, anim_y_cb);
                lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                lv_anim_start(&a);
            }
        }

        if (i == skip_anim_idx) {
            /* 该项会在外部从右侧滑入,不要覆盖 x */
        } else {
            int cur_x = lv_obj_get_x(r);
            if (cur_x != target_x) {
                anim_to(r, cur_x, target_x, 180, lv_anim_path_ease_out, NULL);
            } else {
                lv_obj_set_x(r, target_x);
            }
        }
        y += h + UI_TOAST_GAP;
    }
}

static void toast_relayout(ui_dev_t *d)
{
    toast_relayout_ex(d, -1);
}

static void toast_slide_out_ready(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)lv_anim_get_user_data(a);
    if (obj) lv_obj_del(obj);
}

static void toast_remove_at(ui_dev_t *d, int idx)
{
    if (idx < 0 || idx >= d->toast_count) return;
    if (d->toasts[idx].timer) {
        lv_timer_del(d->toasts[idx].timer);
        d->toasts[idx].timer = NULL;
    }
    lv_obj_t *leaving = d->toasts[idx].root;

    /* 先从数组中移走,后续 relayout 不再涉及它;它本身用动画自删 */
    for (int i = idx; i < d->toast_count - 1; ++i) {
        d->toasts[i] = d->toasts[i + 1];
    }
    d->toast_count--;
    d->toasts[d->toast_count].root  = NULL;
    d->toasts[d->toast_count].timer = NULL;

    if (leaving) {
        /* 取消可能在身上的进入动画,以当前 x 为起点向右滑出 */
        lv_anim_delete(leaving, anim_x_cb);
        int cur_x = lv_obj_get_x(leaving);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, leaving);
        lv_anim_set_values(&a, cur_x, d->W);
        lv_anim_set_time(&a, 180);
        lv_anim_set_exec_cb(&a, anim_x_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        lv_anim_set_user_data(&a, leaving);
        lv_anim_set_ready_cb(&a, toast_slide_out_ready);
        lv_anim_start(&a);
    }
    toast_relayout(d);
}

esp_err_t ui_toast_push(ui_dev_t *dev, ui_toast_level_e level,
                        const char *text, uint32_t auto_ms)
{
    if (!dev || !dev->initialized || !text) return ESP_ERR_INVALID_ARG;
    if (level > UI_TOAST_ERROR) level = UI_TOAST_INFO;
    if (!ui_lock(dev)) return ESP_ERR_TIMEOUT;

    /* 满则丢最旧 */
    if (dev->toast_count >= UI_TOAST_MAX) {
        toast_remove_at(dev, 0);
    }

    uint32_t border_color, bg_color;
    switch (level) {
        case UI_TOAST_WARN:  border_color = 0xFF9800; bg_color = 0x2A1A00; break;
        case UI_TOAST_ERROR: border_color = 0xF44336; bg_color = 0x300000; break;
        case UI_TOAST_INFO:
        default:             border_color = 0x2962FF; bg_color = 0x001028; break;
    }

    lv_obj_t *root = lv_obj_create(lv_layer_top());
    lv_obj_set_width(root, UI_TOAST_W);
    lv_obj_set_height(root, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color    (root, lv_color_hex(bg_color),     0);
    lv_obj_set_style_bg_opa      (root, LV_OPA_COVER,               0);
    lv_obj_set_style_border_color(root, lv_color_hex(border_color), 0);
    lv_obj_set_style_border_width(root, 2, 0);
    lv_obj_set_style_radius      (root, 0, 0);
    lv_obj_set_style_pad_all     (root, UI_TOAST_PAD, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(root);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, UI_TOAST_W - 2 * UI_TOAST_PAD - 4);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);

    int slot = dev->toast_count++;
    dev->toasts[slot].root  = root;
    dev->toasts[slot].level = level;
    dev->toasts[slot].timer = NULL;

    /* 先放到屏幕右侧外位置,然后由 toast_relayout_ex 跳过本项 X 动画,
     * 本函数额外手动启动从 W → 目标 X 的滑入动画 */
    lv_obj_set_x(root, dev->W);

    /* INFO/WARN 自动消失;ERROR 持久 */
    if (level != UI_TOAST_ERROR) {
        uint32_t period = auto_ms ? auto_ms :
                          (level == UI_TOAST_WARN ? 5000 : 3000);
        lv_timer_t *t = lv_timer_create(toast_timer_cb, period, dev);
        lv_timer_set_repeat_count(t, 1);
        dev->toasts[slot].timer = t;
    }

    toast_relayout_ex(dev, slot);
    /* 启动本项从右侧外滑入 */
    int target_x = dev->W - UI_TOAST_W - UI_TOAST_X_OFF;
    anim_to(root, dev->W, target_x, 220, lv_anim_path_ease_out, NULL);
    /* 保证状态栏与底栏仍在最前 */
    lv_obj_move_foreground(dev->sb_root);
    lv_obj_move_foreground(dev->bot_root);
    ui_unlock(dev);
    return ESP_OK;
}

esp_err_t ui_toast_clear_errors(ui_dev_t *dev)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    if (!ui_lock(dev)) return ESP_ERR_TIMEOUT;
    /* 从后向前删,索引稳定 */
    for (int i = dev->toast_count - 1; i >= 0; --i) {
        if (dev->toasts[i].level == UI_TOAST_ERROR) {
            toast_remove_at(dev, i);
        }
    }
    ui_unlock(dev);
    return ESP_OK;
}
