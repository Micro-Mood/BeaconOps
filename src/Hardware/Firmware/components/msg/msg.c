/**
 * @file msg.c
 * @brief 消息子系统实现 — msg_clear 工具 + msg_dev_t 顶层调度器
 *
 * 调度循环(默认 100 ms tick):每 tick 在锁内填充 msg_intent_t,释锁后执行回调。
 *   step_sweep    — TTL 扫除非当前项(收集 EXPIRED ack)
 *   step_current  — 首屏 DISPLAYED ack;当前 TTL 到期且非 EMERG → 弹出
 *   step_shake    — pending shake 受 min_display_ms 保护后 ack 弹出
 *   step_pick     — 空闲选 top;有当前但 top 更高级且 min_display_ms 已过 → 抢占
 * aging / lru_flush 按墙钟秒差驱动,而非 tick 计数 — light sleep 唤醒后会自动补齐。
 *
 * 用户 cb 在持锁外调用(intent 模式),与外部子系统(ui/audio/tx)解耦,
 * 调用方无需考虑 msg 锁的重入风险。
 */

#include "msg.h"
#include "pa_mqs.h"
#include "lru_dedup.h"
#include "parser.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "msg";

/* ---- 默认值 ------------------------------------------------------------ */
#define MSG_DEF_MIN_DISPLAY_MS     2000
#define MSG_DEF_TICK_PERIOD_MS     100
#define MSG_DEF_LRU_FLUSH_PERIOD_S 300
#define MSG_DEF_TASK_PRIO          (tskIDLE_PRIORITY + 4)
#define MSG_DEF_TASK_STACK         4096

#define MSG_STORE_DEF_PREFIX       "/spiffs/mq_"
#define MSG_STORE_DEF_MAX          64
#define MSG_STORE_SUFFIX_LIVE      ".j"
#define MSG_STORE_SUFFIX_ACKED     ".a"
#define MSG_STORE_SUFFIX_EXPIRED   ".e"
#define MSG_STORE_PATH_CAP         320
#define MSG_STORE_NAME_CAP          96
#define MSG_STORE_FINAL_REPLAY_MS  5000

/* ============================================================
 *  msg_clear(数据工具)
 * ============================================================ */

void msg_clear(msg_t *m)
{
    if (!m) return;
    free(m->title);
    free(m->body);
    free(m->audio_text);
    memset(m, 0, sizeof(*m));
}

/* ============================================================
 *  msg_dev_t 句柄
 * ============================================================ */

struct msg_dev_s {
    msg_config_t       cfg;
    char              *spiffs_lru_path;     /* strdup,可 NULL */
    char              *spiffs_msg_prefix;   /* strdup;空字符串表示禁用 */

    SemaphoreHandle_t  mu;
    TaskHandle_t       task;

    pa_mqs_ctx_t       mqs;
    lru_dedup_ctx_t    lru;

    /* 当前显示状态 */
    char     current_id[37];
    int      current_level;
    uint32_t current_shown_at_ms;
    bool     current_displayed_acked;
    volatile uint32_t pending_shake_ms;

    char     last_stack_ids[MSG_UI_STACK_MAX][37];
    size_t   last_stack_n;

    /* 墙钟驱动的 aging / lru flush 时间戳(单位:unix 秒,0=未初始化) */
    uint32_t last_aging_unix;
    uint32_t last_flush_unix;
    uint32_t last_final_replay_ms;

    uint32_t store_count;
    uint16_t store_seq;

    uint32_t drop_count;
    volatile bool stop;
    bool     initialized;
};

/* ---- 时间小工具 -------------------------------------------------------- */

static uint32_t now_ms(void)   { return (uint32_t)(esp_timer_get_time() / 1000ULL); }
static uint32_t now_unix(void) { time_t t = 0; time(&t); return (uint32_t)t; }

/* ---- 调度意图(锁内填充,锁外执行) -------------------------------------- */

#define MSG_INTENT_ACK_MAX   (PA_MQS_CAP + 2)

typedef struct {
    char           id[37];
    msg_ack_kind_e kind;
} msg_ack_item_t;

typedef struct {
    msg_ack_item_t acks[MSG_INTENT_ACK_MAX];
    int            ack_n;
    bool           dismiss;          /* 触发 ui_dismiss */
    bool           confirm;          /* 当前消息被用户确认 */
    bool           show;             /* 触发 ui_show + on_tts */
    msg_t          show_msg;         /* 独占 strdup;调用完 msg_clear */
    bool           stack_update;
    msg_t          stack[MSG_UI_STACK_MAX];
    size_t         stack_n;
} msg_intent_t;

typedef struct {
    msg_dev_t    *dev;
    msg_intent_t *intent;
} msg_expire_ctx_t;
/* ---- 前向声明 ---------------------------------------------------------- */
static void msg_task_fn       (void *arg);
static void msg_step_sweep    (msg_dev_t *d, uint32_t t_unix, msg_intent_t *it);
static void msg_step_current  (msg_dev_t *d, uint32_t t_unix, msg_intent_t *it);
static void msg_step_shake    (msg_dev_t *d, uint32_t t_ms,   msg_intent_t *it);
static void msg_step_pick     (msg_dev_t *d, uint32_t t_ms,   msg_intent_t *it);
static void msg_present_locked(msg_dev_t *d, const msg_t *m,
                               uint32_t t_ms, msg_intent_t *it);
static void msg_clear_current (msg_dev_t *d);
static void msg_collect_expired_cb(const msg_t *m, void *user);
static void msg_intent_add_ack(msg_intent_t *it, const char *id, msg_ack_kind_e k);
static void msg_step_stack    (msg_dev_t *d, msg_intent_t *it);
static void msg_intent_clear  (msg_intent_t *it);

static bool      msg_store_enabled      (msg_dev_t *d);
static uint32_t  msg_store_count_files  (msg_dev_t *d);
static void      msg_store_enforce_cap  (msg_dev_t *d);
static esp_err_t msg_store_save_locked  (msg_dev_t *d, const char *id,
                                         const char *payload, int len);
static esp_err_t msg_store_mark_locked  (msg_dev_t *d, const char *id,
                                         msg_ack_kind_e kind);
static void      msg_store_delete_locked(msg_dev_t *d, const char *id);
static void      msg_store_load_locked  (msg_dev_t *d);
static void      msg_store_replay_final_locked(msg_dev_t *d, msg_intent_t *it);

/* ============================================================
 *  init / deinit
 * ============================================================ */

esp_err_t msg_init(msg_dev_t **dev, const msg_config_t *config)
{
    if (!dev || !config || !config->ui_show || !config->ui_dismiss) return ESP_ERR_INVALID_ARG;

    msg_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;

    d->cfg = *config;
    if (d->cfg.min_display_ms     == 0) d->cfg.min_display_ms     = MSG_DEF_MIN_DISPLAY_MS;
    if (d->cfg.tick_period_ms     == 0) d->cfg.tick_period_ms     = MSG_DEF_TICK_PERIOD_MS;
    if (d->cfg.lru_flush_period_s == 0) d->cfg.lru_flush_period_s = MSG_DEF_LRU_FLUSH_PERIOD_S;
    if (d->cfg.msg_store_max      == 0) d->cfg.msg_store_max      = MSG_STORE_DEF_MAX;
    if (d->cfg.task_prio          == 0) d->cfg.task_prio          = MSG_DEF_TASK_PRIO;
    if (d->cfg.task_stack         == 0) d->cfg.task_stack         = MSG_DEF_TASK_STACK;

    if (config->spiffs_lru_path) {
        d->spiffs_lru_path = strdup(config->spiffs_lru_path);
        if (!d->spiffs_lru_path) { ESP_LOGE(TAG, "init: strdup lru_path"); goto fail; }
    }

    const char *msg_prefix = config->spiffs_msg_prefix;
    if (!msg_prefix) msg_prefix = MSG_STORE_DEF_PREFIX;
    d->spiffs_msg_prefix = strdup(msg_prefix);
    if (!d->spiffs_msg_prefix) { ESP_LOGE(TAG, "init: strdup msg_prefix"); goto fail; }

    d->mu = xSemaphoreCreateMutex();
    if (!d->mu) { ESP_LOGE(TAG, "init: xSemaphoreCreateMutex"); goto fail; }

    esp_err_t er;
    if ((er = pa_mqs_init(&d->mqs)) != ESP_OK) {
        ESP_LOGE(TAG, "init: pa_mqs_init=%s", esp_err_to_name(er)); goto fail;
    }
    if ((er = lru_dedup_init(&d->lru, d->spiffs_lru_path)) != ESP_OK) {
        ESP_LOGE(TAG, "init: lru_dedup_init(%s)=%s",
                 d->spiffs_lru_path ? d->spiffs_lru_path : "(null)",
                 esp_err_to_name(er));
        goto fail_mqs;
    }

    if (msg_store_enabled(d)) {
        d->store_count = msg_store_count_files(d);
        msg_store_enforce_cap(d);
        msg_store_load_locked(d);
    }

    BaseType_t r = xTaskCreate(msg_task_fn, "msg",
                               d->cfg.task_stack, d,
                               d->cfg.task_prio, &d->task);
    if (r != pdPASS) { ESP_LOGE(TAG, "init: xTaskCreate stack=%lu prio=%lu free_heap=%lu",
                                (unsigned long)d->cfg.task_stack,
                                (unsigned long)d->cfg.task_prio,
                                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT)); goto fail_lru; }

    d->initialized = true;
    *dev = d;
    ESP_LOGI(TAG, "msg 初始化完成 (min_display=%lums, tick=%lums)",
             (unsigned long)d->cfg.min_display_ms,
             (unsigned long)d->cfg.tick_period_ms);
    return ESP_OK;

fail_lru:
    lru_dedup_deinit(&d->lru);
fail_mqs:
    pa_mqs_deinit(&d->mqs);
fail:
    if (d->mu)              vSemaphoreDelete(d->mu);
    if (d->spiffs_lru_path) free(d->spiffs_lru_path);
    if (d->spiffs_msg_prefix) free(d->spiffs_msg_prefix);
    free(d);
    *dev = NULL;
    return ESP_FAIL;
}

esp_err_t msg_deinit(msg_dev_t **dev)
{
    if (!dev || !*dev) return ESP_ERR_INVALID_ARG;
    msg_dev_t *d = *dev;
    if (!d->initialized) return ESP_ERR_INVALID_STATE;

    d->stop = true;
    if (d->task) {
        xTaskNotifyGive(d->task);
        for (int i = 0; i < 200 && eTaskGetState(d->task) != eDeleted; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (eTaskGetState(d->task) != eDeleted) vTaskDelete(d->task);
        d->task = NULL;
    }

    /* 落盘 LRU */
    if (d->mu) {
        xSemaphoreTake(d->mu, portMAX_DELAY);
        (void)lru_dedup_flush(&d->lru);
        xSemaphoreGive(d->mu);
    }

    lru_dedup_deinit(&d->lru);
    pa_mqs_deinit(&d->mqs);

    if (d->mu)              { vSemaphoreDelete(d->mu); d->mu = NULL; }
    if (d->spiffs_lru_path) free(d->spiffs_lru_path);
    if (d->spiffs_msg_prefix) free(d->spiffs_msg_prefix);
    d->initialized = false;
    free(d);
    *dev = NULL;
    ESP_LOGI(TAG, "msg 反初始化完成");
    return ESP_OK;
}

/* ============================================================
 *  公共 API
 * ============================================================ */

esp_err_t msg_ingest(msg_dev_t *dev, const char *payload, int len)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;

    msg_t m = {0};
    esp_err_t r = msg_parse(payload, len, &m);
    if (r != ESP_OK) {
        dev->drop_count++;
        return r;
    }

    if (xSemaphoreTake(dev->mu, pdMS_TO_TICKS(200)) != pdTRUE) {
        msg_clear(&m);
        dev->drop_count++;
        return ESP_ERR_TIMEOUT;
    }

    /* dedup:仅探测,不写入。admit 成功后才登记,避免"队列满 / 优先级不够 →
     * admit 失败但 token 已 +1"的永封问题(服务端 QoS1 重发同 id 也会被吃掉)。 */
    if (lru_dedup_seen(&dev->lru, m.id)) {
        ESP_LOGI(TAG, "dedup 命中 id=%s", m.id);
        char id_copy[37];
        strlcpy(id_copy, m.id, sizeof(id_copy));
        msg_clear(&m);
        xSemaphoreGive(dev->mu);
        if (dev->cfg.on_ack) {
            dev->cfg.on_ack(id_copy, MSG_ACK_RECEIVED, dev->cfg.ack_user);
        }
        return ESP_OK;
    }

    /* admit 会零化 m,先备份 id 用于 dedup 登记 */
    char id_copy[37];
    strlcpy(id_copy, m.id, sizeof(id_copy));
    int notify_level = (int)m.level;

    esp_err_t sr = msg_store_save_locked(dev, id_copy, payload, len);
    if (sr != ESP_OK && msg_store_enabled(dev)) {
        ESP_LOGW(TAG, "store 保存失败 id=%s (%s)", id_copy, esp_err_to_name(sr));
        msg_clear(&m);
        dev->drop_count++;
        xSemaphoreGive(dev->mu);
        return sr;
    }

    bool emit_received = false;
    bool emit_expired = false;
    bool emit_notify = false;
    bool notify_task = false;

    r = pa_mqs_admit(&dev->mqs, &m, now_unix());
    if (r == ESP_OK) {
        lru_dedup_add(&dev->lru, id_copy);
        emit_received = true;
        emit_notify = true;
        notify_task = true;
    } else if (msg_store_enabled(dev)) {
        lru_dedup_add(&dev->lru, id_copy);
        emit_received = true;
        if (r == ESP_ERR_TIMEOUT) {
            msg_store_mark_locked(dev, id_copy, MSG_ACK_EXPIRED);
            emit_expired = true;
        }
    }
    xSemaphoreGive(dev->mu);

    if (r != ESP_OK) {
        ESP_LOGW(TAG, "admit 拒收 (%s)", esp_err_to_name(r));
        msg_clear(&m);
        if (!emit_received) {
            dev->drop_count++;
            return r;
        }
    }

    if (dev->cfg.on_ack && emit_received) {
        dev->cfg.on_ack(id_copy, MSG_ACK_RECEIVED, dev->cfg.ack_user);
    }
    if (dev->cfg.on_ack && emit_expired) {
        dev->cfg.on_ack(id_copy, MSG_ACK_EXPIRED, dev->cfg.ack_user);
    }
    if (dev->cfg.on_notify && emit_notify) {
        dev->cfg.on_notify(notify_level, dev->cfg.notify_user);
    }
    /* 唤醒调度任务,无需等下一 tick */
    if (notify_task && dev->task) xTaskNotifyGive(dev->task);
    return ESP_OK;
}

void msg_on_shake(msg_dev_t *dev)
{
    if (!dev || !dev->initialized) return;
    uint32_t t_ms = now_ms();
    dev->pending_shake_ms = t_ms ? t_ms : 1;
    /* 立即唤醒调度任务 — light sleep 期间 tick 会被拉长,
     * 仅靠 ulTaskNotifyTake 超时会有数百 ms 抖动。 */
    if (dev->task) xTaskNotifyGive(dev->task);
}

void msg_on_ack_delivered(msg_dev_t *dev, const char *msg_id,
                          msg_ack_kind_e kind)
{
    if (!dev || !dev->initialized || !msg_id || !*msg_id) return;
    if (kind != MSG_ACK_ACKED && kind != MSG_ACK_EXPIRED) return;
    if (xSemaphoreTake(dev->mu, pdMS_TO_TICKS(200)) != pdTRUE) return;
    msg_store_delete_locked(dev, msg_id);
    msg_store_load_locked(dev);
    xSemaphoreGive(dev->mu);
    if (dev->task) xTaskNotifyGive(dev->task);
}

uint32_t msg_get_drop_count(msg_dev_t *dev)
{
    return (dev && dev->initialized) ? dev->drop_count : 0;
}

/* ============================================================
 *  调度任务
 * ============================================================ */

static void msg_task_fn(void *arg)
{
    msg_dev_t *d = (msg_dev_t *)arg;

    while (!d->stop) {
        uint32_t t_unix = now_unix();
        uint32_t t_ms   = now_ms();
        msg_intent_t it; memset(&it, 0, sizeof(it));

        if (xSemaphoreTake(d->mu, pdMS_TO_TICKS(50)) == pdTRUE) {
            msg_step_sweep  (d, t_unix, &it);
            msg_step_current(d, t_unix, &it);
            msg_step_shake  (d, t_ms,   &it);
            msg_step_pick   (d, t_ms,   &it);

            /* 老化按墙钟秒差驱动:light sleep 期间 tick 会被压扁/拉长,
             * 用 unix 秒差做增量,唤醒后一次补齐,避免 aging_bonus 与现实脱节。
             * SNTP 未同步时 t_unix 很小,此时 last_aging_unix 也很小,
             * 差值仍合理(只是首次同步那一刻可能补一个大跳变 — 由 AGING_CAP 兜底)。 */
            if (d->last_aging_unix == 0) {
                d->last_aging_unix = t_unix;
            } else if (t_unix > d->last_aging_unix) {
                pa_mqs_tick_aging(&d->mqs, t_unix - d->last_aging_unix);
                d->last_aging_unix = t_unix;
            }

            if (d->last_flush_unix == 0) {
                d->last_flush_unix = t_unix;
            } else if (t_unix - d->last_flush_unix >= d->cfg.lru_flush_period_s) {
                d->last_flush_unix = t_unix;
                (void)lru_dedup_flush(&d->lru);
            }

            if (d->last_final_replay_ms == 0 ||
                (uint32_t)(t_ms - d->last_final_replay_ms) >= MSG_STORE_FINAL_REPLAY_MS) {
                d->last_final_replay_ms = t_ms;
                msg_store_replay_final_locked(d, &it);
            }

            msg_step_stack(d, &it);

            xSemaphoreGive(d->mu);
        }

        /* ---- 锁外执行回调 ---- */
        for (int i = 0; i < it.ack_n; ++i) {
            if (d->cfg.on_ack) {
                d->cfg.on_ack(it.acks[i].id, it.acks[i].kind, d->cfg.ack_user);
            }
        }
        if (it.dismiss && d->cfg.ui_dismiss) {
            if (!d->cfg.ui_stack) d->cfg.ui_dismiss(d->cfg.ui_user);
        }
        if (it.confirm && d->cfg.on_confirm) {
            d->cfg.on_confirm(d->cfg.confirm_user);
        }
        if (it.show) {
            if (!d->cfg.ui_stack) {
                d->cfg.ui_show(it.show_msg.title ? it.show_msg.title : "",
                               it.show_msg.body  ? it.show_msg.body  : "",
                               (int)it.show_msg.level, d->cfg.ui_user);
            }
            if (d->cfg.on_tts && it.show_msg.audio_text && *it.show_msg.audio_text) {
                d->cfg.on_tts(it.show_msg.audio_text,
                              (int)it.show_msg.level, d->cfg.tts_user);
            }
        }

        if (it.stack_update && d->cfg.ui_stack) {
            msg_ui_card_view_t cards[MSG_UI_STACK_MAX];
            for (size_t i = 0; i < it.stack_n; ++i) {
                cards[i].id    = it.stack[i].id;
                cards[i].title = it.stack[i].title ? it.stack[i].title : "";
                cards[i].body  = it.stack[i].body  ? it.stack[i].body  : "";
                cards[i].level = (int)it.stack[i].level;
            }
            d->cfg.ui_stack(cards, it.stack_n, d->cfg.ui_user);
        }

        msg_intent_clear(&it);

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(d->cfg.tick_period_ms));
    }
    vTaskDelete(NULL);
}

/* ============================================================
 *  调度 step(全部已持 mu;只产出 intent,不直接调 cb)
 * ============================================================ */

static void msg_intent_add_ack(msg_intent_t *it, const char *id, msg_ack_kind_e k)
{
    if (!id || !*id || it->ack_n >= MSG_INTENT_ACK_MAX) return;
    for (int i = 0; i < it->ack_n; ++i) {
        if (it->acks[i].kind == k && strcmp(it->acks[i].id, id) == 0) return;
    }
    strlcpy(it->acks[it->ack_n].id, id, sizeof(it->acks[it->ack_n].id));
    it->acks[it->ack_n].kind = k;
    it->ack_n++;
}

static void msg_deep_copy(msg_t *dst, const msg_t *src)
{
    *dst = *src;
    dst->title      = src->title      ? strdup(src->title)      : NULL;
    dst->body       = src->body       ? strdup(src->body)       : NULL;
    dst->audio_text = src->audio_text ? strdup(src->audio_text) : NULL;
}

static bool msg_id_in_selected(const msg_t *const selected[], size_t n, const char *id)
{
    if (!id || !*id) return false;
    for (size_t i = 0; i < n; ++i) {
        if (selected[i] && strcmp(selected[i]->id, id) == 0) return true;
    }
    return false;
}

static const msg_t *msg_pick_stack_next(msg_dev_t *d, const msg_t *const selected[], size_t n)
{
    const msg_t *best = NULL;
    uint32_t best_score = 0;
    uint32_t best_ts = UINT32_MAX;
    for (size_t i = 0; i < PA_MQS_CAP; ++i) {
        const msg_t *s = pa_mqs_get_at(&d->mqs, i);
        if (!s || msg_id_in_selected(selected, n, s->id)) continue;
        uint32_t score = pa_mqs_score(s);
        if (!best || score > best_score ||
            (score == best_score && s->enqueue_ts < best_ts)) {
            best = s;
            best_score = score;
            best_ts = s->enqueue_ts;
        }
    }
    return best;
}

static void msg_step_stack(msg_dev_t *d, msg_intent_t *it)
{
    const msg_t *selected[MSG_UI_STACK_MAX] = {0};
    size_t n = 0;

    if (d->current_id[0]) {
        for (size_t i = 0; i < PA_MQS_CAP; ++i) {
            const msg_t *s = pa_mqs_get_at(&d->mqs, i);
            if (s && strcmp(s->id, d->current_id) == 0) {
                selected[n++] = s;
                break;
            }
        }
    }
    while (n < MSG_UI_STACK_MAX) {
        const msg_t *next = msg_pick_stack_next(d, selected, n);
        if (!next) break;
        selected[n++] = next;
    }

    bool changed = (n != d->last_stack_n);
    if (!changed) {
        for (size_t i = 0; i < n; ++i) {
            if (strcmp(d->last_stack_ids[i], selected[i]->id) != 0) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return;

    it->stack_update = true;
    it->stack_n = n;
    d->last_stack_n = n;
    for (size_t i = 0; i < MSG_UI_STACK_MAX; ++i) d->last_stack_ids[i][0] = '\0';
    for (size_t i = 0; i < n; ++i) {
        msg_deep_copy(&it->stack[i], selected[i]);
        strlcpy(d->last_stack_ids[i], selected[i]->id, sizeof(d->last_stack_ids[i]));
    }
}

static void msg_intent_clear(msg_intent_t *it)
{
    msg_clear(&it->show_msg);
    for (size_t i = 0; i < it->stack_n; ++i) msg_clear(&it->stack[i]);
}

static void msg_collect_expired_cb(const msg_t *m, void *user)
{
    msg_expire_ctx_t *ctx = (msg_expire_ctx_t *)user;
    if (!ctx || !ctx->intent || !m || !m->id[0]) return;
    msg_intent_add_ack(ctx->intent, m->id, MSG_ACK_EXPIRED);
    if (ctx->dev) msg_store_mark_locked(ctx->dev, m->id, MSG_ACK_EXPIRED);
}

static void msg_step_sweep(msg_dev_t *d, uint32_t t_unix, msg_intent_t *it)
{
    /* SNTP 未同步:expire_ts 不可信(parser 已置 0,但旧消息可能残留),整张跳过 */
    if (t_unix < 1700000000u) return;
    msg_expire_ctx_t ctx = { .dev = d, .intent = it };
    size_t removed = pa_mqs_sweep_expired(&d->mqs, t_unix, d->current_id,
                                          msg_collect_expired_cb, &ctx);
    if (removed) msg_store_load_locked(d);
}

static void msg_step_current(msg_dev_t *d, uint32_t t_unix, msg_intent_t *it)
{
    if (!d->current_id[0]) return;

    /* 找当前槽(须先于 DISPLAYED ack,以便按 ack_mode 过滤) */
    const msg_t *cur_slot = NULL;
    for (size_t i = 0; i < PA_MQS_CAP; ++i) {
        const msg_t *s = pa_mqs_get_at(&d->mqs, i);
        if (s && strcmp(s->id, d->current_id) == 0) { cur_slot = s; break; }
    }
    if (!cur_slot) {
        /* 已被外部移除 */
        it->dismiss = true;
        msg_clear_current(d);
        return;
    }

    /* 首屏 DISPLAYED ack(只发一次,且仅 ack_mode >= DISPLAYED 才上报) */
    if (!d->current_displayed_acked) {
        d->current_displayed_acked = true;
        if (cur_slot->ack_mode >= MSG_ACK_MODE_DISPLAYED) {
            msg_intent_add_ack(it, d->current_id, MSG_ACK_DISPLAYED);
        }
    }
    if (cur_slot->level == MSG_LEVEL_EMERG)            return;
    if (cur_slot->expire_ts == 0)                      return;
    if (t_unix < 1700000000u)                          return;   /* SNTP 未同步,不判过期 */
    if (cur_slot->expire_ts > t_unix)                  return;

    ESP_LOGI(TAG, "current %s 过期", d->current_id);
    msg_intent_add_ack(it, d->current_id, MSG_ACK_EXPIRED);
    msg_store_mark_locked(d, d->current_id, MSG_ACK_EXPIRED);
    pa_mqs_remove(&d->mqs, d->current_id);
    it->dismiss = true;
    msg_clear_current(d);
    msg_store_load_locked(d);
}

static void msg_step_shake(msg_dev_t *d, uint32_t t_ms, msg_intent_t *it)
{
    uint32_t shake_ms = d->pending_shake_ms;
    if (!shake_ms) return;

    /* 本 tick 已 dismiss(过期/外部移除) → shake 留到下 tick 再判,
     * 防止与刚被 pick 的新卡发生跨卡 stale-shake 误 ack */
    if (it->dismiss) return;

    if (!d->current_id[0]) {
        /* 空闲屏幕的摇晃,目前无意义,丢弃 */
        d->pending_shake_ms = 0;
        return;
    }
    if ((int32_t)(shake_ms - d->current_shown_at_ms) < 0) {
        /* 这是上一张卡遗留的 shake,不能用于当前卡 */
        d->pending_shake_ms = 0;
        return;
    }
    uint32_t elapsed = t_ms - d->current_shown_at_ms;
    if (elapsed < d->cfg.min_display_ms) return;   /* 下 tick 再试 */

    /* 找当前槽以检查 ack_mode */
    const msg_t *shake_slot = NULL;
    for (size_t i = 0; i < PA_MQS_CAP; ++i) {
        const msg_t *s = pa_mqs_get_at(&d->mqs, i);
        if (s && strcmp(s->id, d->current_id) == 0) { shake_slot = s; break; }
    }

    ESP_LOGI(TAG, "shake-ack %s", d->current_id);
    /* 仅 ack_mode >= ACKNOWLEDGED 时才向服务端上报 ACKED */
    if (!shake_slot || shake_slot->ack_mode >= MSG_ACK_MODE_ACKNOWLEDGED) {
        msg_intent_add_ack(it, d->current_id, MSG_ACK_ACKED);
    }
    msg_store_mark_locked(d, d->current_id, MSG_ACK_ACKED);
    pa_mqs_remove(&d->mqs, d->current_id);
    it->dismiss = true;
    it->confirm = true;
    msg_clear_current(d);
    msg_store_load_locked(d);
    d->pending_shake_ms = 0;
}

static void msg_step_pick(msg_dev_t *d, uint32_t t_ms, msg_intent_t *it)
{
    if (!d->current_id[0]) {
        const msg_t *top = pa_mqs_pick_best(&d->mqs, NULL);
        if (top) msg_present_locked(d, top, t_ms, it);
        return;
    }
    /* 抢占检查 */
    const msg_t *top = pa_mqs_pick_best(&d->mqs, d->current_id);
    if (!top || (int)top->level <= d->current_level) return;
    uint32_t elapsed = t_ms - d->current_shown_at_ms;
    if (elapsed < d->cfg.min_display_ms) return;

    ESP_LOGI(TAG, "抢占 %s -> %s (lv %d->%d)",
             d->current_id, top->id,
             d->current_level, (int)top->level);
    msg_present_locked(d, top, t_ms, it);
}

/* ============================================================
 *  辅助(已持 mu)
 * ============================================================ */

static void msg_present_locked(msg_dev_t *d, const msg_t *m,
                               uint32_t t_ms, msg_intent_t *it)
{
    if (!m) return;
    /* 深拷贝到 intent;ui_show / on_tts 在锁外执行,完成后由调用方 msg_clear */
    msg_deep_copy(&it->show_msg, m);
    it->show = true;

    strlcpy(d->current_id, m->id, sizeof(d->current_id));
    d->current_level           = (int)m->level;
    d->current_shown_at_ms     = t_ms;
    d->current_displayed_acked = false;

    uint32_t shake_ms = d->pending_shake_ms;
    if (shake_ms && (int32_t)(shake_ms - t_ms) < 0) {
        /* 换 current 时只清历史 shake;若 transition 期间来了新 shake,
         * 它的时间戳会晚于本轮 t_ms,保留给下一 tick 处理。 */
        d->pending_shake_ms = 0;
    }
}

static void msg_clear_current(msg_dev_t *d)
{
    d->current_id[0]            = '\0';
    d->current_level            = 0;
    d->current_shown_at_ms      = 0;
    d->current_displayed_acked  = false;
}

/* ============================================================
 *  SPIFFS 消息持久化
 *
 *  文件状态:
 *    <prefix><unix>_<seq>_<hash>.j  已接收,待显示/待确认
 *    <prefix><unix>_<seq>_<hash>.a  用户已摇晃,ACKED 待送达服务端
 *    <prefix><unix>_<seq>_<hash>.e  已过期,EXPIRED 待送达服务端
 * ============================================================ */

typedef struct {
    char     path[MSG_STORE_PATH_CAP];
    uint32_t ts;
    uint32_t seq;
} msg_store_file_t;

static bool msg_store_enabled(msg_dev_t *d)
{
    return d && d->spiffs_msg_prefix && d->spiffs_msg_prefix[0];
}

static void msg_store_split_prefix(msg_dev_t *d, char *dir, size_t dir_cap,
                                   char *name_prefix, size_t name_cap)
{
    const char *p = d->spiffs_msg_prefix;
    const char *slash = strrchr(p, '/');
    if (!slash) {
        strlcpy(dir, ".", dir_cap);
        strlcpy(name_prefix, p, name_cap);
        return;
    }
    size_t dir_len = (size_t)(slash - p);
    if (dir_len == 0) dir_len = 1;
    if (dir_len >= dir_cap) dir_len = dir_cap - 1;
    memcpy(dir, p, dir_len);
    dir[dir_len] = '\0';
    strlcpy(name_prefix, slash + 1, name_cap);
}

static bool msg_store_suffix(const char *name, const char *suffix)
{
    size_t nl = strlen(name);
    size_t sl = strlen(suffix);
    return nl > sl && memcmp(name + nl - sl, suffix, sl) == 0;
}

static bool msg_store_match_name(const char *name, const char *prefix,
                                 bool live_only, uint32_t *ts, uint32_t *seq)
{
    size_t pl = strlen(prefix);
    if (strncmp(name, prefix, pl) != 0) return false;
    bool live = msg_store_suffix(name, MSG_STORE_SUFFIX_LIVE);
    if (live_only && !live) return false;
    if (!live && !msg_store_suffix(name, MSG_STORE_SUFFIX_ACKED) &&
        !msg_store_suffix(name, MSG_STORE_SUFFIX_EXPIRED)) {
        return false;
    }

    const char *p = name + pl;
    char *end = NULL;
    unsigned long t = strtoul(p, &end, 10);
    if (!end || *end != '_') return false;
    unsigned long s = strtoul(end + 1, &end, 16);
    if (!end || *end != '_') return false;
    unsigned long h = strtoul(end + 1, &end, 16);
    (void)h;
    if (!end || *end != '.') return false;
    if (ts) *ts = (uint32_t)t;
    if (seq) *seq = (uint32_t)s;
    return true;
}

static uint32_t msg_store_hash_id(const char *id)
{
    uint32_t h = 2166136261u;
    if (!id) return h;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h;
}

static bool msg_store_read_id(const char *path, char *id, size_t id_cap)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    struct stat st;
    if (fstat(fileno(fp), &st) != 0 || st.st_size <= 0) {
        fclose(fp);
        return false;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { fclose(fp); return false; }
    size_t rd = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (rd != (size_t)st.st_size) { free(buf); return false; }
    buf[rd] = '\0';
    msg_t m = {0};
    bool ok = (msg_parse(buf, (int)rd, &m) == ESP_OK && m.id[0]);
    if (ok) strlcpy(id, m.id, id_cap);
    msg_clear(&m);
    free(buf);
    return ok;
}

static bool msg_store_file_has_id(const char *path, const char *id)
{
    char file_id[37];
    return msg_store_read_id(path, file_id, sizeof(file_id)) && strcmp(file_id, id) == 0;
}

static bool msg_store_extract_final(const char *name, const char *prefix,
                                    const char *path,
                                    char *id, size_t id_cap,
                                    msg_ack_kind_e *kind)
{
    if (msg_store_suffix(name, MSG_STORE_SUFFIX_ACKED)) {
        if (kind) *kind = MSG_ACK_ACKED;
    } else if (msg_store_suffix(name, MSG_STORE_SUFFIX_EXPIRED)) {
        if (kind) *kind = MSG_ACK_EXPIRED;
    } else {
        return false;
    }
    if (!msg_store_match_name(name, prefix, false, NULL, NULL)) return false;
    return msg_store_read_id(path, id, id_cap);
}

static int msg_store_find_by_id(msg_dev_t *d, const char *id, bool live_only,
                                char *out, size_t cap)
{
    if (!msg_store_enabled(d) || !id || !*id) return -1;
    char dir[64], prefix[32];
    msg_store_split_prefix(d, dir, sizeof(dir), prefix, sizeof(prefix));
    DIR *dp = opendir(dir);
    if (!dp) return -1;
    struct dirent *ent;
    int found = -1;
    while ((ent = readdir(dp)) != NULL) {
        if (!msg_store_match_name(ent->d_name, prefix, live_only, NULL, NULL)) continue;
        char path[MSG_STORE_PATH_CAP];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (!msg_store_file_has_id(path, id)) continue;
        strlcpy(out, path, cap);
        found = 0;
        break;
    }
    closedir(dp);
    return found;
}

static uint32_t msg_store_count_files(msg_dev_t *d)
{
    if (!msg_store_enabled(d)) return 0;
    char dir[64], prefix[32];
    msg_store_split_prefix(d, dir, sizeof(dir), prefix, sizeof(prefix));
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    uint32_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (msg_store_match_name(ent->d_name, prefix, false, NULL, NULL)) n++;
    }
    closedir(dp);
    return n;
}

static int msg_store_scan_oldest(msg_dev_t *d, char *out, size_t cap)
{
    char dir[64], prefix[32];
    msg_store_split_prefix(d, dir, sizeof(dir), prefix, sizeof(prefix));
    DIR *dp = opendir(dir);
    if (!dp) return -1;
    uint32_t best_ts = UINT32_MAX, best_seq = UINT32_MAX;
    char best[MSG_STORE_NAME_CAP] = {0};
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        uint32_t ts = 0, seq = 0;
        if (!msg_store_match_name(ent->d_name, prefix, false, &ts, &seq)) continue;
        if (ts < best_ts || (ts == best_ts && seq < best_seq)) {
            best_ts = ts;
            best_seq = seq;
            strlcpy(best, ent->d_name, sizeof(best));
        }
    }
    closedir(dp);
    if (!best[0]) return -1;
    snprintf(out, cap, "%s/%s", dir, best);
    return 0;
}

static void msg_store_enforce_cap(msg_dev_t *d)
{
    if (!msg_store_enabled(d)) return;
    while (d->store_count > d->cfg.msg_store_max) {
        char path[MSG_STORE_PATH_CAP];
        if (msg_store_scan_oldest(d, path, sizeof(path)) != 0) break;
        if (unlink(path) == 0) {
            d->store_count--;
            ESP_LOGW(TAG, "store: 容量满,删 %s", path);
        } else {
            break;
        }
    }
}

static esp_err_t msg_store_save_locked(msg_dev_t *d, const char *id,
                                       const char *payload, int len)
{
    if (!msg_store_enabled(d)) return ESP_OK;
    if (!id || !*id || !payload || len <= 0) return ESP_ERR_INVALID_ARG;

    char path[MSG_STORE_PATH_CAP];
    bool exists = (msg_store_find_by_id(d, id, false, path, sizeof(path)) == 0);
    if (!exists) {
        uint32_t ts = now_unix();
        uint32_t hash = msg_store_hash_id(id);
        for (int tries = 0; tries < 8; ++tries) {
            uint16_t seq = ++d->store_seq;
            snprintf(path, sizeof(path), "%s%010lu_%04x_%08lx%s",
                     d->spiffs_msg_prefix,
                     (unsigned long)ts, (unsigned)seq,
                     (unsigned long)hash, MSG_STORE_SUFFIX_LIVE);
            struct stat st;
            if (stat(path, &st) != 0) break;
        }
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) return ESP_FAIL;
    size_t w = fwrite(payload, 1, (size_t)len, fp);
    fclose(fp);
    if (w != (size_t)len) {
        unlink(path);
        return ESP_FAIL;
    }
    if (!exists) d->store_count++;
    msg_store_enforce_cap(d);
    return ESP_OK;
}

static esp_err_t msg_store_mark_locked(msg_dev_t *d, const char *id,
                                       msg_ack_kind_e kind)
{
    if (!msg_store_enabled(d)) return ESP_OK;
    const char *suffix = NULL;
    if (kind == MSG_ACK_ACKED) suffix = MSG_STORE_SUFFIX_ACKED;
    else if (kind == MSG_ACK_EXPIRED) suffix = MSG_STORE_SUFFIX_EXPIRED;
    else return ESP_OK;

    char old_path[MSG_STORE_PATH_CAP];
    if (msg_store_find_by_id(d, id, true, old_path, sizeof(old_path)) != 0) return ESP_OK;
    char new_path[MSG_STORE_PATH_CAP];
    strlcpy(new_path, old_path, sizeof(new_path));
    char *dot = strrchr(new_path, '.');
    if (!dot) return ESP_FAIL;
    strlcpy(dot, suffix, sizeof(new_path) - (size_t)(dot - new_path));
    if (strcmp(old_path, new_path) == 0) return ESP_OK;
    (void)unlink(new_path);
    return (rename(old_path, new_path) == 0) ? ESP_OK : ESP_FAIL;
}

static void msg_store_delete_locked(msg_dev_t *d, const char *id)
{
    if (!msg_store_enabled(d) || !id || !*id) return;
    char path[MSG_STORE_PATH_CAP];
    while (msg_store_find_by_id(d, id, false, path, sizeof(path)) == 0) {
        if (unlink(path) != 0) break;
        if (d->store_count) d->store_count--;
        ESP_LOGI(TAG, "store: 删除 %s", path);
    }
}

static int msg_store_file_cmp(const void *a, const void *b)
{
    const msg_store_file_t *fa = (const msg_store_file_t *)a;
    const msg_store_file_t *fb = (const msg_store_file_t *)b;
    if (fa->ts != fb->ts) return (fa->ts < fb->ts) ? -1 : 1;
    if (fa->seq != fb->seq) return (fa->seq < fb->seq) ? -1 : 1;
    return strcmp(fa->path, fb->path);
}

static void msg_store_load_locked(msg_dev_t *d)
{
    if (!msg_store_enabled(d)) return;
    char dir[64], prefix[32];
    msg_store_split_prefix(d, dir, sizeof(dir), prefix, sizeof(prefix));

    DIR *dp = opendir(dir);
    if (!dp) return;
    uint32_t cap = d->store_count ? d->store_count : 1;
    msg_store_file_t *files = calloc(cap, sizeof(*files));
    if (!files) { closedir(dp); return; }

    uint32_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        uint32_t ts = 0, seq = 0;
        if (!msg_store_match_name(ent->d_name, prefix, true, &ts, &seq)) continue;
        if (n >= cap) break;
        snprintf(files[n].path, sizeof(files[n].path), "%s/%s", dir, ent->d_name);
        files[n].ts = ts;
        files[n].seq = seq;
        n++;
    }
    closedir(dp);
    qsort(files, n, sizeof(*files), msg_store_file_cmp);

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < n; ++i) {
        FILE *fp = fopen(files[i].path, "rb");
        if (!fp) continue;
        struct stat st;
        if (fstat(fileno(fp), &st) != 0 || st.st_size <= 0) {
            fclose(fp);
            continue;
        }
        char *buf = malloc((size_t)st.st_size + 1);
        if (!buf) { fclose(fp); break; }
        size_t rd = fread(buf, 1, (size_t)st.st_size, fp);
        fclose(fp);
        if (rd != (size_t)st.st_size) { free(buf); continue; }
        buf[rd] = '\0';

        msg_t m = {0};
        if (msg_parse(buf, (int)rd, &m) == ESP_OK) {
            lru_dedup_add(&d->lru, m.id);
            if (!pa_mqs_contains(&d->mqs, m.id)) {
                esp_err_t r = pa_mqs_admit(&d->mqs, &m, now_unix());
                if (r == ESP_OK) loaded++;
                else msg_clear(&m);
            } else {
                msg_clear(&m);
            }
        }
        free(buf);
    }
    free(files);
    if (loaded) ESP_LOGI(TAG, "store: 恢复 %lu 条消息", (unsigned long)loaded);
}

static void msg_store_replay_final_locked(msg_dev_t *d, msg_intent_t *it)
{
    if (!msg_store_enabled(d) || !it) return;
    char dir[64], prefix[32];
    msg_store_split_prefix(d, dir, sizeof(dir), prefix, sizeof(prefix));
    DIR *dp = opendir(dir);
    if (!dp) return;

    struct dirent *ent;
    while (it->ack_n < MSG_INTENT_ACK_MAX && (ent = readdir(dp)) != NULL) {
        char path[MSG_STORE_PATH_CAP];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        char id[37];
        msg_ack_kind_e kind = MSG_ACK_ACKED;
        if (!msg_store_extract_final(ent->d_name, prefix, path, id, sizeof(id), &kind)) continue;
        msg_intent_add_ack(it, id, kind);
    }
    closedir(dp);
}
