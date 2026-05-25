/**
 * @file tx.c
 * @brief 出站 ack 服务实现
 */

#include "tx.h"
#include "mqtt.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "tx";

/* ---- 默认值 ------------------------------------------------------------ */
#define TX_DEF_NAMESPACE             "tx_pending"
#define TX_NVS_KEY_BLOB              "ring"
#define TX_DEF_MAX_PENDING           32
#define TX_DEF_MAX_PUBLISH_PER_TICK  4
#define TX_DEF_DRAIN_PERIOD_MS       5000
#define TX_DEF_MAX_ATTEMPTS          10
#define TX_DEF_BACKOFF_BASE_MS       2000u
#define TX_DEF_BACKOFF_MAX_MS        300000u
#define TX_DEF_TASK_PRIO             (tskIDLE_PRIORITY + 2)
#define TX_DEF_TASK_STACK            3072

#define TX_BLOB_VERSION              2u
#define TX_PAYLOAD_CAP               240
#define TX_ID_CAP                    37

/* ---- 持久 entry(packed,跨升级稳定) -------------------------------- */
typedef struct __attribute__((packed)) {
    char     id[TX_ID_CAP];
    uint8_t  kind;
    uint8_t  attempts;
    uint8_t  failed;
    uint8_t  _pad;
    uint32_t ts;
    uint32_t next_due_ms;
} tx_pending_entry_t;

typedef struct __attribute__((packed)) {
    char     id[TX_ID_CAP];
    uint8_t  kind;
    uint8_t  _pad[3];
    uint32_t ts;
} tx_pending_entry_v1_t;

/* ---- 句柄 -------------------------------------------------------------- */
struct tx_dev_s {
    tx_config_t          cfg;
    char                *nvs_namespace;   /* strdup */

    SemaphoreHandle_t    mu;
    TaskHandle_t         task;

    uint32_t             count;
    uint32_t             capacity;
    tx_pending_entry_t  *entries;         /* malloc(capacity * sizeof(*)) */

    bool                 dirty;
    uint32_t             fail_count;

    volatile bool        stop;
    bool                 initialized;
};

/* ---- 前向声明 ---------------------------------------------------------- */
static void        tx_drain_task(void *arg);
static esp_err_t   tx_nvs_load_locked(tx_dev_t *dev);
static esp_err_t   tx_nvs_save_locked(tx_dev_t *dev);
static bool        tx_ring_push_locked     (tx_dev_t *dev, const tx_pending_entry_t *e);
static void        tx_ring_remove_at_locked(tx_dev_t *dev, uint32_t i);
static int         tx_build_payload(const tx_pending_entry_t *e, char *out, size_t cap);
static const char *tx_kind_str (tx_ack_kind_e k);
static int         tx_kind_qos (tx_ack_kind_e k);
static uint32_t    tx_now_ms(void);
static uint32_t    tx_backoff_ms(uint8_t attempts);
static void        tx_emit_result(tx_dev_t *dev, const tx_pending_entry_t *e, bool ok);

/* ============================================================
 *  init / deinit
 * ============================================================ */

esp_err_t tx_init(tx_dev_t **dev, const tx_config_t *config)
{
    if (!dev || !config || !config->mqtt) return ESP_ERR_INVALID_ARG;

    tx_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;

    d->cfg = *config;
    if (d->cfg.max_pending          == 0) d->cfg.max_pending          = TX_DEF_MAX_PENDING;
    if (d->cfg.max_publish_per_tick == 0) d->cfg.max_publish_per_tick = TX_DEF_MAX_PUBLISH_PER_TICK;
    if (d->cfg.drain_period_ms      == 0) d->cfg.drain_period_ms      = TX_DEF_DRAIN_PERIOD_MS;
    if (d->cfg.max_attempts         == 0) d->cfg.max_attempts         = TX_DEF_MAX_ATTEMPTS;
    if (d->cfg.task_prio            == 0) d->cfg.task_prio            = TX_DEF_TASK_PRIO;
    if (d->cfg.task_stack           == 0) d->cfg.task_stack           = TX_DEF_TASK_STACK;

    const char *ns = d->cfg.nvs_namespace ? d->cfg.nvs_namespace : TX_DEF_NAMESPACE;
    d->nvs_namespace = strdup(ns);
    if (!d->nvs_namespace) goto fail;

    d->capacity = d->cfg.max_pending;
    d->entries  = calloc(d->capacity, sizeof(*d->entries));
    if (!d->entries) goto fail;

    d->mu = xSemaphoreCreateMutex();
    if (!d->mu) goto fail;

    /* 加载持久 ring(失败不算致命) */
    xSemaphoreTake(d->mu, portMAX_DELAY);
    esp_err_t lr = tx_nvs_load_locked(d);
    xSemaphoreGive(d->mu);
    if (lr != ESP_OK && lr != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_load 失败: %s(空 ring 启动)", esp_err_to_name(lr));
    }

    BaseType_t r = xTaskCreate(tx_drain_task, "tx_drain",
                               d->cfg.task_stack, d,
                               d->cfg.task_prio, &d->task);
    if (r != pdPASS) goto fail;

    d->initialized = true;
    *dev = d;
    ESP_LOGI(TAG, "tx 初始化完成 (cap=%lu, pending=%lu, drain=%lums)",
             (unsigned long)d->capacity, (unsigned long)d->count,
             (unsigned long)d->cfg.drain_period_ms);
    return ESP_OK;

fail:
    if (d->mu)            vSemaphoreDelete(d->mu);
    if (d->entries)       free(d->entries);
    if (d->nvs_namespace) free(d->nvs_namespace);
    free(d);
    *dev = NULL;
    return ESP_FAIL;
}

esp_err_t tx_deinit(tx_dev_t **dev)
{
    if (!dev || !*dev) return ESP_ERR_INVALID_ARG;
    tx_dev_t *d = *dev;
    if (!d->initialized) return ESP_ERR_INVALID_STATE;

    d->stop = true;
    if (d->task) {
        xTaskNotifyGive(d->task);
        for (int i = 0; i < 200 && eTaskGetState(d->task) != eDeleted; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (eTaskGetState(d->task) != eDeleted) {
            vTaskDelete(d->task);
        }
        d->task = NULL;
    }

    /* 持久化最终状态 */
    if (d->mu) {
        xSemaphoreTake(d->mu, portMAX_DELAY);
        if (d->dirty) (void)tx_nvs_save_locked(d);
        xSemaphoreGive(d->mu);
        vSemaphoreDelete(d->mu);
        d->mu = NULL;
    }

    if (d->entries)       free(d->entries);
    if (d->nvs_namespace) free(d->nvs_namespace);
    d->initialized = false;
    free(d);
    *dev = NULL;
    ESP_LOGI(TAG, "tx 反初始化完成");
    return ESP_OK;
}

/* ============================================================
 *  公共 API
 * ============================================================ */

esp_err_t tx_emit_ack(tx_dev_t *dev, const char *msg_id, tx_ack_kind_e kind)
{
    if (!dev || !dev->initialized)        return ESP_ERR_INVALID_STATE;
    if (!msg_id || !*msg_id)              return ESP_ERR_INVALID_ARG;

    tx_pending_entry_t e = {0};
    strlcpy(e.id, msg_id, sizeof(e.id));
    e.kind = (uint8_t)kind;
    time_t t = 0; time(&t);
    e.ts = (uint32_t)t;

    if (xSemaphoreTake(dev->mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    bool changed = tx_ring_push_locked(dev, &e);
    esp_err_t r = changed ? tx_nvs_save_locked(dev) : ESP_OK;
    xSemaphoreGive(dev->mu);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "nvs_save 失败: %s(数据仍在 RAM)", esp_err_to_name(r));
    }

    /* immediate flush(尽力即可) */
    if (mqtt_is_connected(dev->cfg.mqtt)) (void)tx_flush(dev);
    if (dev->task) xTaskNotifyGive(dev->task);
    return ESP_OK;
}

esp_err_t tx_flush(tx_dev_t *dev)
{
    if (!dev || !dev->initialized)         return ESP_ERR_INVALID_STATE;
    if (!mqtt_is_connected(dev->cfg.mqtt)) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(dev->mu, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t now_ms = tx_now_ms();
    int published = 0;
    uint32_t i = 0;
    while (i < dev->count && published < (int)dev->cfg.max_publish_per_tick) {
        tx_pending_entry_t e = dev->entries[i];
        if (e.failed) { i++; continue; }
        if (e.next_due_ms && (int32_t)(now_ms - e.next_due_ms) < 0) { i++; continue; }

        char payload[TX_PAYLOAD_CAP];
        int n = tx_build_payload(&e, payload, sizeof(payload));
        if (n <= 0 || n >= (int)sizeof(payload)) {
            ESP_LOGW(TAG, "payload build 失败 id=%s,丢弃", e.id);
            tx_ring_remove_at_locked(dev, i);
            continue;
        }
        esp_err_t r = mqtt_publish_ack(dev->cfg.mqtt, payload,
                                       tx_kind_qos((tx_ack_kind_e)e.kind));
        if (r != ESP_OK) {
            if (dev->entries[i].attempts < UINT8_MAX) dev->entries[i].attempts++;
            dev->entries[i].next_due_ms = now_ms + tx_backoff_ms(dev->entries[i].attempts);
            dev->fail_count++;
            dev->dirty = true;
            if (dev->entries[i].attempts >= dev->cfg.max_attempts) {
                dev->entries[i].failed = 1;
                ESP_LOGW(TAG, "ack %s/%s 超过 %u 次,标记失败",
                         e.id, tx_kind_str((tx_ack_kind_e)e.kind),
                         (unsigned)dev->cfg.max_attempts);
                tx_emit_result(dev, &dev->entries[i], false);
            } else {
                ESP_LOGW(TAG, "publish_ack 失败(%s),%u 次后退避 %lums",
                         esp_err_to_name(r), (unsigned)dev->entries[i].attempts,
                         (unsigned long)tx_backoff_ms(dev->entries[i].attempts));
            }
            i++;
            continue;
        }
        tx_emit_result(dev, &e, true);
        tx_ring_remove_at_locked(dev, i);
        published++;
    }

    esp_err_t saver = ESP_OK;
    if (dev->dirty) saver = tx_nvs_save_locked(dev);
    uint32_t remain = dev->count;
    xSemaphoreGive(dev->mu);

    if (published > 0) {
        ESP_LOGI(TAG, "flush %d 条;剩余 %lu", published, (unsigned long)remain);
    }
    return saver;
}

size_t tx_pending_count(tx_dev_t *dev)
{
    /* 32-bit 读取在 RISC-V 上原子,允许无锁快查 */
    if (!dev || !dev->initialized) return 0;
    return (size_t)dev->count;
}

uint32_t tx_get_fail_count(tx_dev_t *dev)
{
    return (dev && dev->initialized) ? dev->fail_count : 0;
}

/* ============================================================
 *  drain task
 * ============================================================ */

static void tx_drain_task(void *arg)
{
    tx_dev_t *dev = (tx_dev_t *)arg;
    while (!dev->stop) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(dev->cfg.drain_period_ms));
        if (dev->stop) break;
        if (dev->count > 0 && mqtt_is_connected(dev->cfg.mqtt)) {
            (void)tx_flush(dev);
        }
    }
    vTaskDelete(NULL);
}

/* ============================================================
 *  ring(caller 持 mu)
 * ============================================================ */

static bool tx_ring_push_locked(tx_dev_t *dev, const tx_pending_entry_t *e)
{
    for (uint32_t i = 0; i < dev->count; ++i) {
        if (strcmp(dev->entries[i].id, e->id) == 0 && dev->entries[i].kind == e->kind) {
            return false;
        }
    }
    if (dev->count >= dev->capacity) {
        ESP_LOGW(TAG, "ring 满,丢最老 %s", dev->entries[0].id);
        tx_ring_remove_at_locked(dev, 0);
    }
    dev->entries[dev->count++] = *e;
    dev->dirty = true;
    return true;
}

static void tx_ring_remove_at_locked(tx_dev_t *dev, uint32_t i)
{
    if (i >= dev->count) return;
    for (uint32_t j = i; j + 1 < dev->count; ++j) {
        dev->entries[j] = dev->entries[j + 1];
    }
    dev->count--;
    dev->dirty = true;
}

/* ============================================================
 *  NVS 持久化(caller 持 mu)
 * ============================================================ */

static esp_err_t tx_nvs_load_locked(tx_dev_t *dev)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(dev->nvs_namespace, NVS_READONLY, &h);
    if (r == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (r != ESP_OK) return r;

    /* 先读 size,再决定是否能容纳 */
    size_t sz = 0;
    r = nvs_get_blob(h, TX_NVS_KEY_BLOB, NULL, &sz);
    if (r == ESP_ERR_NVS_NOT_FOUND) { nvs_close(h); return ESP_OK; }
    if (r != ESP_OK)                { nvs_close(h); return r; }
    if (sz < sizeof(uint32_t) * 2)  { nvs_close(h); return ESP_OK; }

    uint8_t *buf = malloc(sz);
    if (!buf) { nvs_close(h); return ESP_ERR_NO_MEM; }
    r = nvs_get_blob(h, TX_NVS_KEY_BLOB, buf, &sz);
    nvs_close(h);
    if (r != ESP_OK) { free(buf); return r; }

    uint32_t version, count;
    memcpy(&version, buf, sizeof(version));
    memcpy(&count,   buf + sizeof(version), sizeof(count));
    if (version != 1u && version != TX_BLOB_VERSION) {
        ESP_LOGW(TAG, "blob 版本不兼容(got=%lu),丢弃",
                 (unsigned long)version);
        free(buf);
        return ESP_OK;
    }
    if (count > dev->capacity) {
        ESP_LOGW(TAG, "blob count=%lu 超容量 %lu,截断",
                 (unsigned long)count, (unsigned long)dev->capacity);
        count = dev->capacity;
    }
    size_t entry_sz = (version == 1u) ? sizeof(tx_pending_entry_v1_t)
                                      : sizeof(tx_pending_entry_t);
    size_t need = sizeof(version) + sizeof(count) + (size_t)count * entry_sz;
    if (need > sz) {
        ESP_LOGW(TAG, "blob 截断不完整(need=%zu, got=%zu),丢弃", need, sz);
        free(buf);
        return ESP_OK;
    }
    if (version == 1u) {
        tx_pending_entry_v1_t *old = (tx_pending_entry_v1_t *)(buf + sizeof(version) + sizeof(count));
        for (uint32_t i = 0; i < count; ++i) {
            strlcpy(dev->entries[i].id, old[i].id, sizeof(dev->entries[i].id));
            dev->entries[i].kind = old[i].kind;
            dev->entries[i].ts = old[i].ts;
        }
        dev->dirty = true;
    } else {
        memcpy(dev->entries, buf + sizeof(version) + sizeof(count),
               (size_t)count * sizeof(tx_pending_entry_t));
        dev->dirty = false;
    }
    dev->count = count;
    free(buf);
    ESP_LOGI(TAG, "加载 %lu 条 pending ack", (unsigned long)dev->count);
    return ESP_OK;
}

static esp_err_t tx_nvs_save_locked(tx_dev_t *dev)
{
    size_t sz = sizeof(uint32_t) * 2
              + (size_t)dev->count * sizeof(tx_pending_entry_t);
    uint8_t *buf = malloc(sz);
    if (!buf) return ESP_ERR_NO_MEM;
    uint32_t version = TX_BLOB_VERSION;
    memcpy(buf,                        &version,    sizeof(version));
    memcpy(buf + sizeof(version),      &dev->count, sizeof(dev->count));
    memcpy(buf + sizeof(version) * 2,  dev->entries,
           (size_t)dev->count * sizeof(tx_pending_entry_t));

    nvs_handle_t h;
    esp_err_t r = nvs_open(dev->nvs_namespace, NVS_READWRITE, &h);
    if (r != ESP_OK) { free(buf); return r; }
    r = nvs_set_blob(h, TX_NVS_KEY_BLOB, buf, sz);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    free(buf);
    if (r == ESP_OK) dev->dirty = false;
    return r;
}

/* ============================================================
 *  小工具
 * ============================================================ */

static int tx_build_payload(const tx_pending_entry_t *e, char *out, size_t cap)
{
    /* protocol v1: {"msg_id":"...","kind":"...","ts":N} */
    return snprintf(out, cap,
                    "{\"msg_id\":\"%s\",\"kind\":\"%s\",\"ts\":%u}",
                    e->id, tx_kind_str((tx_ack_kind_e)e->kind),
                    (unsigned)e->ts);
}

static const char *tx_kind_str(tx_ack_kind_e k)
{
    switch (k) {
        case TX_ACK_RECEIVED:  return "received";
        case TX_ACK_DISPLAYED: return "displayed";
        case TX_ACK_ACKED:     return "acknowledged";
        case TX_ACK_EXPIRED:   return "expired";
        default:               return "unknown";
    }
}

static int tx_kind_qos(tx_ack_kind_e k)
{
    return (k == TX_ACK_ACKED) ? 2 : 1;   /* spec §6 */
}

static uint32_t tx_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t tx_backoff_ms(uint8_t attempts)
{
    if (attempts == 0) return 0;
    uint32_t shift = attempts > 8 ? 8 : (uint32_t)(attempts - 1);
    uint32_t ms = TX_DEF_BACKOFF_BASE_MS << shift;
    return ms > TX_DEF_BACKOFF_MAX_MS ? TX_DEF_BACKOFF_MAX_MS : ms;
}

static void tx_emit_result(tx_dev_t *dev, const tx_pending_entry_t *e, bool ok)
{
    if (dev->cfg.on_result && e && e->id[0]) {
        dev->cfg.on_result(e->id, (tx_ack_kind_e)e->kind, ok,
                           dev->cfg.result_user);
    }
}
