/**
 * @file error.c
 * @brief 统一错误诊断组件实现
 */

#include "error.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static const char *TAG = "error";

/* ---- 内部默认值 -------------------------------------------------------- */
#define ERR_DEF_LOG_DIR         "/spiffs/diag"
#define ERR_DEF_LOG_NAME        "errors.log"
#define ERR_DEF_RING_DEPTH      32
#define ERR_DEF_FLUSH_MS        60000U
#define ERR_DEF_FILE_MAX_BYTES  4096U
#define ERR_DEF_FILE_KEEP       2
#define ERR_DEF_TASK_PRIO       (tskIDLE_PRIORITY + 1)
#define ERR_DEF_TASK_STACK      2048

/* ---- 内部结构 ---------------------------------------------------------- */
struct err_dev_s {
    err_config_t       cfg;
    char               log_path[96];   ///< "<dir>/errors.log"
    err_entry_t       *ring;
    uint16_t           ring_depth;
    uint16_t           head;           ///< 下一个写入位置
    uint16_t           count;          ///< 当前条目数(<= ring_depth)
    uint16_t           pending_flush;  ///< 自上次 flush 起新增条目数
    SemaphoreHandle_t  mtx;
    TaskHandle_t       task;
    volatile bool      stop;
    uint32_t           spiffs_fail_count;
    bool               initialized;
};

/* ---- 前向声明 ---------------------------------------------------------- */
static void     err_worker_task(void *arg);
static esp_err_t err_flush_locked(err_dev_t *dev);
static esp_err_t err_rotate_if_needed(err_dev_t *dev);
static int      err_format_entry(const err_entry_t *e, char *out, size_t out_len);
static err_entry_t *err_take_oldest_for_read(err_dev_t *dev, uint16_t i);

/* ============================================================
 *  公共 API
 * ============================================================ */

esp_err_t err_init(err_dev_t **dev, const err_config_t *config)
{
    if (!dev || !config) return ESP_ERR_INVALID_ARG;

    err_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;

    d->cfg = *config;

    /* 默认值回填 */
    if (d->cfg.log_dir         == NULL) d->cfg.log_dir         = ERR_DEF_LOG_DIR;
    if (d->cfg.ring_depth      == 0)    d->cfg.ring_depth      = ERR_DEF_RING_DEPTH;
    if (d->cfg.flush_period_ms == 0)    d->cfg.flush_period_ms = ERR_DEF_FLUSH_MS;
    if (d->cfg.file_max_bytes  == 0)    d->cfg.file_max_bytes  = ERR_DEF_FILE_MAX_BYTES;
    if (d->cfg.file_keep       == 0)    d->cfg.file_keep       = ERR_DEF_FILE_KEEP;
    if (d->cfg.task_prio       == 0)    d->cfg.task_prio       = ERR_DEF_TASK_PRIO;
    if (d->cfg.task_stack      == 0)    d->cfg.task_stack      = ERR_DEF_TASK_STACK;

    d->ring_depth = d->cfg.ring_depth;

    /* 拼日志全路径 */
    snprintf(d->log_path, sizeof(d->log_path), "%s/%s",
             d->cfg.log_dir, ERR_DEF_LOG_NAME);

    /* 资源分配 */
    d->ring = calloc(d->ring_depth, sizeof(err_entry_t));
    if (!d->ring) goto fail;

    d->mtx = xSemaphoreCreateMutex();
    if (!d->mtx) goto fail;

    /* worker task — 仅在启用 SPIFFS 且 flush_period_ms != UINT32_MAX 时启动 */
    if (d->cfg.spiffs && d->cfg.flush_period_ms != UINT32_MAX) {
        BaseType_t r = xTaskCreate(err_worker_task, "err_flush",
                                   d->cfg.task_stack, d,
                                   d->cfg.task_prio, &d->task);
        if (r != pdPASS) goto fail;
    }

    d->initialized = true;
    *dev = d;
    ESP_LOGI(TAG, "error 初始化完成 (ring=%u, spiffs=%s, flush=%lums)",
             (unsigned)d->ring_depth,
             d->cfg.spiffs ? "on" : "off",
             (unsigned long)d->cfg.flush_period_ms);
    return ESP_OK;

fail:
    if (d->task) vTaskDelete(d->task);
    if (d->mtx)  vSemaphoreDelete(d->mtx);
    if (d->ring) free(d->ring);
    free(d);
    *dev = NULL;
    return ESP_FAIL;
}

esp_err_t err_deinit(err_dev_t **dev)
{
    if (!dev || !*dev) return ESP_ERR_INVALID_ARG;
    err_dev_t *d = *dev;
    if (!d->initialized) return ESP_ERR_INVALID_STATE;

    /* 停 worker */
    if (d->task) {
        d->stop = true;
        xTaskNotifyGive(d->task);
        /* 给 worker 一点时间自行退出;最多等 200ms 后强删 */
        for (int i = 0; i < 20 && eTaskGetState(d->task) != eDeleted; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (eTaskGetState(d->task) != eDeleted) {
            vTaskDelete(d->task);
        }
        d->task = NULL;
    }

    /* 最后 flush 一次 */
    if (d->cfg.spiffs && d->mtx) {
        xSemaphoreTake(d->mtx, portMAX_DELAY);
        err_flush_locked(d);
        xSemaphoreGive(d->mtx);
    }

    if (d->mtx)  vSemaphoreDelete(d->mtx);
    if (d->ring) free(d->ring);

    d->initialized = false;
    free(d);
    *dev = NULL;
    ESP_LOGI(TAG, "error 反初始化完成");
    return ESP_OK;
}

esp_err_t err_record(err_dev_t *dev,
                     err_level_e lv,
                     const char *tag,
                     esp_err_t code,
                     const char *fmt, ...)
{
    /* 早期阶段 dev=NULL,降级为只走 ESP_LOG */
    if (!dev || !dev->initialized) {
        if (fmt) {
            va_list ap;
            va_start(ap, fmt);
            esp_log_writev(ESP_LOG_INFO, tag ? tag : "?", fmt, ap);
            va_end(ap);
        }
        return ESP_OK;
    }

    /* 准备一条记录(在栈上格式化,避免持锁太久) */
    err_entry_t e;
    memset(&e, 0, sizeof(e));
    e.ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
    e.level = lv;
    e.code  = code;

    if (tag) {
        strncpy(e.tag, tag, sizeof(e.tag) - 1);
        e.tag[sizeof(e.tag) - 1] = '\0';
    } else {
        strcpy(e.tag, "?");
    }

    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
        va_end(ap);
    }

    /* 入环形缓冲(覆盖式) */
    if (xSemaphoreTake(dev->mtx, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    dev->ring[dev->head] = e;
    dev->head = (dev->head + 1) % dev->ring_depth;
    if (dev->count < dev->ring_depth) dev->count++;
    if (dev->pending_flush < dev->ring_depth) dev->pending_flush++;
    xSemaphoreGive(dev->mtx);

    /* 同步打印一份到串口,便于 OpenOCD/UART 抓取 */
    esp_log_level_t esp_lv;
    switch (lv) {
        case ERR_LV_DEBUG: esp_lv = ESP_LOG_DEBUG; break;
        case ERR_LV_INFO:  esp_lv = ESP_LOG_INFO;  break;
        case ERR_LV_WARN:  esp_lv = ESP_LOG_WARN;  break;
        case ERR_LV_ERROR:
        case ERR_LV_FATAL: esp_lv = ESP_LOG_ERROR; break;
        default:           esp_lv = ESP_LOG_INFO;  break;
    }
    if (code != ESP_OK) {
        ESP_LOG_LEVEL(esp_lv, e.tag, "[%s] %s (code=%d:%s)",
                      err_to_string(code), e.msg,
                      (int)code, esp_err_to_name(code));
    } else {
        ESP_LOG_LEVEL(esp_lv, e.tag, "%s", e.msg);
    }

    /* FATAL 立即触发 worker 立刻 flush */
    if (lv >= ERR_LV_ERROR && dev->task) {
        xTaskNotifyGive(dev->task);
    }
    return ESP_OK;
}

esp_err_t err_flush(err_dev_t *dev)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    if (!dev->cfg.spiffs) return ESP_OK;   /* 未启用持久化 */
    if (xSemaphoreTake(dev->mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = err_flush_locked(dev);
    xSemaphoreGive(dev->mtx);
    return ret;
}

size_t err_recent(err_dev_t *dev, char *out, size_t out_len)
{
    if (!dev || !dev->initialized || !out || out_len == 0) return 0;

    if (xSemaphoreTake(dev->mtx, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    size_t written = 0;
    out[0] = '\0';

    /* 从最旧到最新输出 */
    uint16_t n = dev->count;
    for (uint16_t i = 0; i < n; i++) {
        const err_entry_t *e = err_take_oldest_for_read(dev, i);
        if (!e) break;
        char line[256];
        int len = err_format_entry(e, line, sizeof(line));
        if (len <= 0) continue;
        if (written + (size_t)len + 1 >= out_len) break;
        memcpy(out + written, line, (size_t)len);
        written += (size_t)len;
        out[written++] = '\n';
    }
    if (written < out_len) out[written] = '\0';
    else                   out[out_len - 1] = '\0';

    xSemaphoreGive(dev->mtx);
    return written;
}

const char *err_to_string(esp_err_t code)
{
    return esp_err_to_name(code);
}

char err_level_char(err_level_e lv)
{
    switch (lv) {
        case ERR_LV_DEBUG: return 'D';
        case ERR_LV_INFO:  return 'I';
        case ERR_LV_WARN:  return 'W';
        case ERR_LV_ERROR: return 'E';
        case ERR_LV_FATAL: return 'F';
        default:           return '?';
    }
}

/* ============================================================
 *  内部工具
 * ============================================================ */

/**
 * @brief 取环形缓冲第 i 个最旧条目(只读)
 */
static err_entry_t *err_take_oldest_for_read(err_dev_t *dev, uint16_t i)
{
    if (i >= dev->count) return NULL;
    uint16_t base = (dev->count == dev->ring_depth) ? dev->head : 0;
    uint16_t idx  = (base + i) % dev->ring_depth;
    return &dev->ring[idx];
}

/**
 * @brief 一行文本格式: "ts.ms L tag code msg"
 */
static int err_format_entry(const err_entry_t *e, char *out, size_t out_len)
{
    return snprintf(out, out_len, "%llu %c %s %d %s",
                    (unsigned long long)e->ts_ms,
                    err_level_char(e->level),
                    e->tag,
                    (int)e->code,
                    e->msg);
}

/**
 * @brief 真正写文件的 flush;调用方须持锁
 */
static esp_err_t err_flush_locked(err_dev_t *dev)
{
    if (dev->pending_flush == 0) return ESP_OK;

    /* 文件尺寸轮转 */
    err_rotate_if_needed(dev);

    FILE *fp = fopen(dev->log_path, "a");
    if (!fp) {
        dev->spiffs_fail_count++;
        return ESP_FAIL;
    }

    /* 只 flush 自上次 flush 以来新增的 pending_flush 条 */
    uint16_t n     = dev->pending_flush;
    uint16_t start = (dev->count >= n) ? (dev->count - n) : 0;

    for (uint16_t i = start; i < dev->count; i++) {
        const err_entry_t *e = err_take_oldest_for_read(dev, i);
        if (!e) break;
        char line[256];
        int len = err_format_entry(e, line, sizeof(line));
        if (len <= 0) continue;
        if (fwrite(line, 1, (size_t)len, fp) != (size_t)len) {
            dev->spiffs_fail_count++;
            break;
        }
        fputc('\n', fp);
    }
    fflush(fp);
    fclose(fp);

    dev->pending_flush = 0;
    return ESP_OK;
}

/**
 * @brief 当前文件超过 file_max_bytes 时执行轮转: log → log.1 → log.2 ...
 */
static esp_err_t err_rotate_if_needed(err_dev_t *dev)
{
    FILE *fp = fopen(dev->log_path, "r");
    if (!fp) return ESP_OK;     /* 还不存在,无需轮转 */

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fclose(fp);

    if (sz < (long)dev->cfg.file_max_bytes) return ESP_OK;

    /* 从最旧的开始往后挪: keep-1 → keep, keep-2 → keep-1, ..., 1 → 2, current → 1 */
    char src[128], dst[128];
    /* 删除最旧的 */
    snprintf(dst, sizeof(dst), "%s.%u", dev->log_path, (unsigned)dev->cfg.file_keep);
    remove(dst);
    /* 把 .N-1 → .N, 反向 rename */
    for (int i = dev->cfg.file_keep; i >= 2; i--) {
        snprintf(src, sizeof(src), "%s.%d", dev->log_path, i - 1);
        snprintf(dst, sizeof(dst), "%s.%d", dev->log_path, i);
        rename(src, dst);
    }
    /* 当前 → .1 */
    snprintf(dst, sizeof(dst), "%s.1", dev->log_path);
    if (rename(dev->log_path, dst) != 0) {
        dev->spiffs_fail_count++;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 后台 worker — 周期性 flush + 响应即时通知
 */
static void err_worker_task(void *arg)
{
    err_dev_t *dev = (err_dev_t *)arg;
    const TickType_t period = pdMS_TO_TICKS(dev->cfg.flush_period_ms);

    while (!dev->stop) {
        ulTaskNotifyTake(pdTRUE, period);
        if (dev->stop) break;

        if (xSemaphoreTake(dev->mtx, pdMS_TO_TICKS(200)) == pdTRUE) {
            err_flush_locked(dev);
            xSemaphoreGive(dev->mtx);
        }
    }
    /* 让主线程的 deinit join 检测到 eDeleted */
    vTaskDelete(NULL);
}
