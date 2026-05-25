/**
 * @file profile.c
 * @brief 60 秒滚动行为聚合器实现
 */

#include "profile.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "cJSON.h"

static const char *TAG = "profile";

#ifndef PROFILE_WINDOW_S
#define PROFILE_WINDOW_S    60
#endif

#define PROFILE_DEF_TASK_PRIO   (tskIDLE_PRIORITY + 1)
#define PROFILE_DEF_TASK_STACK  2048

/* SPIFFS 离线队列 —— 一条一文件,文件名 = ts 十进制(字典序 = 时间序).
 * 路径在 SPIFFS 中 “目录” 是假的,只是文件名包含 /;
 * 完整文件名:<base_path>/profile_q_<ts>.json,base_path 交给 VFS 处理. */
#define PROFILE_Q_PREFIX        "profile_q_"
#define PROFILE_Q_SUFFIX        ".json"
#define PROFILE_Q_DEF_MAX       200
#define PROFILE_Q_DEF_DRAIN_N   8

struct profile_dev_s {
    profile_config_t  cfg;
    SemaphoreHandle_t mtx;
    TaskHandle_t      task;

    /* 当前窗口累加器 */
    uint32_t          static_s;
    uint32_t          walk_slow_s;
    uint32_t          walk_fast_s;
    uint32_t          run_s;
    uint32_t          shake_or_fall_s;
    uint32_t          shake_n;
    uint64_t          intensity_sum;
    uint32_t          intensity_samples;

    /* 最新上报的当日步数(不随窗口复位;跨天由上层 reset) */
    int32_t           steps_today;

    /* 监控 */
    uint32_t          fail_count;
    uint32_t          queue_count;     /* 当前 SPIFFS 中积压条数缓存 */

    volatile bool     stop;
    bool              initialized;
};

#define LOCK(d)    xSemaphoreTake((d)->mtx, portMAX_DELAY)
#define UNLOCK(d)  xSemaphoreGive((d)->mtx)

/* ---- 前向声明 ---------------------------------------------------------- */
static void profile_timer_task(void *arg);
static void profile_publish_and_reset(profile_dev_t *dev);
static char *profile_build_json(const profile_delta_t *d);

/* SPIFFS 队列 — 所有 q_* 函数不持 dev->mtx,主调用者 publish_and_reset 以外不调 */
static int  q_make_path(const profile_dev_t *d, int64_t ts, char *out, size_t cap);
static int  q_scan_oldest(const profile_dev_t *d, char *out_path, size_t cap);
static uint32_t q_count_files(const profile_dev_t *d);
static void q_enforce_cap(profile_dev_t *d);
static esp_err_t q_enqueue(profile_dev_t *d, int64_t ts, const char *json);
static uint32_t   q_drain(profile_dev_t *d, uint32_t max_n);

/* ============================================================
 *  init / deinit
 * ============================================================ */

esp_err_t profile_init(profile_dev_t **dev, const profile_config_t *config)
{
    if (!dev || !config || !config->publish_fn) return ESP_ERR_INVALID_ARG;

    profile_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;

    d->cfg = *config;
    if (d->cfg.window_s       == 0) d->cfg.window_s       = PROFILE_WINDOW_S;
    if (d->cfg.task_prio      == 0) d->cfg.task_prio      = PROFILE_DEF_TASK_PRIO;
    if (d->cfg.task_stack     == 0) d->cfg.task_stack     = PROFILE_DEF_TASK_STACK;
    if (d->cfg.queue_max      == 0) d->cfg.queue_max      = PROFILE_Q_DEF_MAX;
    if (d->cfg.drain_per_tick == 0) d->cfg.drain_per_tick = PROFILE_Q_DEF_DRAIN_N;
    d->steps_today = -1;   /* 未知;主循环 setter 后才上报 */

    d->mtx = xSemaphoreCreateMutex();
    if (!d->mtx) goto fail;

    BaseType_t r = xTaskCreate(profile_timer_task, "profile",
                               d->cfg.task_stack, d,
                               d->cfg.task_prio, &d->task);
    if (r != pdPASS) goto fail;

    /* 启动扫一次 SPIFFS 队列;只更新计数,实际 drain 会在首个 publish 成功后调 */
    if (d->cfg.spiffs) {
        d->queue_count = q_count_files(d);
        if (d->queue_count > 0) {
            ESP_LOGI(TAG, "queue: 发现 %lu 条积压", (unsigned long)d->queue_count);
            q_enforce_cap(d);
        }
    }

    d->initialized = true;
    *dev = d;
    ESP_LOGI(TAG, "profile 初始化完成 (window=%lus)", (unsigned long)d->cfg.window_s);
    return ESP_OK;

fail:
    if (d->mtx) vSemaphoreDelete(d->mtx);
    free(d);
    *dev = NULL;
    return ESP_FAIL;
}

esp_err_t profile_deinit(profile_dev_t **dev)
{
    if (!dev || !*dev) return ESP_ERR_INVALID_ARG;
    profile_dev_t *d = *dev;
    if (!d->initialized) return ESP_ERR_INVALID_STATE;

    d->stop = true;
    if (d->task) {
        xTaskNotifyGive(d->task);   /* 立即唤醒 */
        for (int i = 0; i < 200 && eTaskGetState(d->task) != eDeleted; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (eTaskGetState(d->task) != eDeleted) {
            vTaskDelete(d->task);
        }
        d->task = NULL;
    }
    if (d->mtx) { vSemaphoreDelete(d->mtx); d->mtx = NULL; }

    d->initialized = false;
    free(d);
    *dev = NULL;
    ESP_LOGI(TAG, "profile 反初始化完成");
    return ESP_OK;
}

/* ============================================================
 *  事件入口
 * ============================================================ */

esp_err_t profile_on_behavior(profile_dev_t *dev,
                              behavior_state_e st, int intensity_1_10)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;

    LOCK(dev);
    switch (st) {
        case BEHAVIOR_STATIC:         dev->static_s++;        break;
        case BEHAVIOR_WALK_SLOW:      dev->walk_slow_s++;     break;
        case BEHAVIOR_WALK_FAST:      dev->walk_fast_s++;     break;
        case BEHAVIOR_RUN:            dev->run_s++;           break;
        case BEHAVIOR_SHAKE_OR_FALL:  dev->shake_or_fall_s++; break;
        default: break;   /* 未知枚举,忽略不算错 */
    }
    if (intensity_1_10 > 0) {
        dev->intensity_sum     += (uint32_t)intensity_1_10;
        dev->intensity_samples += 1;
    }
    UNLOCK(dev);
    return ESP_OK;
}

esp_err_t profile_on_shake(profile_dev_t *dev)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    LOCK(dev);
    dev->shake_n++;
    UNLOCK(dev);
    return ESP_OK;
}

esp_err_t profile_set_steps_today(profile_dev_t *dev, int32_t steps)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    LOCK(dev);
    dev->steps_today = steps;
    UNLOCK(dev);
    return ESP_OK;
}

uint32_t profile_get_fail_count(profile_dev_t *dev)
{
    return (dev && dev->initialized) ? dev->fail_count : 0;
}

uint32_t profile_queue_count(profile_dev_t *dev)
{
    return (dev && dev->initialized) ? dev->queue_count : 0;
}

/* ============================================================
 *  内部 — timer + publish
 * ============================================================ */

static void profile_timer_task(void *arg)
{
    profile_dev_t *dev = (profile_dev_t *)arg;
    uint32_t ticks_left = dev->cfg.window_s;

    while (!dev->stop) {
        /* 1 秒间隔,deinit 时被 notify 立刻唤醒 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (dev->stop) break;

        if (--ticks_left == 0) {
            ticks_left = dev->cfg.window_s;
            profile_publish_and_reset(dev);
        }
    }
    vTaskDelete(NULL);
}

static void profile_publish_and_reset(profile_dev_t *dev)
{
    profile_delta_t snap;

    LOCK(dev);
    snap.ts_unix         = (int64_t)time(NULL);
    snap.window_s        = dev->cfg.window_s;
    snap.static_s        = dev->static_s;
    snap.walk_slow_s     = dev->walk_slow_s;
    snap.walk_fast_s     = dev->walk_fast_s;
    snap.run_s           = dev->run_s;
    snap.shake_or_fall_s = dev->shake_or_fall_s;
    snap.shake_n         = dev->shake_n;
    snap.intensity_avg   = dev->intensity_samples
                           ? (uint32_t)(dev->intensity_sum / dev->intensity_samples)
                           : 0;
    snap.steps_today     = dev->steps_today;
    /* 复位(步数不复位,为累计值) */
    dev->static_s = dev->walk_slow_s = dev->walk_fast_s = 0;
    dev->run_s = dev->shake_or_fall_s = dev->shake_n = 0;
    dev->intensity_sum = 0;
    dev->intensity_samples = 0;
    UNLOCK(dev);

    char *payload = profile_build_json(&snap);
    if (!payload) {
        dev->fail_count++;
        ESP_LOGW(TAG, "build JSON 失败 (fail_total=%lu)", (unsigned long)dev->fail_count);
        return;
    }

    /* SNTP 未同步时 ts 不可信,入队会造成文件名冲突 + 服务器无法使用;
     * 该帧直接丢弃(不计 fail_count,因为是客观环境不具备) */
    if (snap.ts_unix < 1700000000LL) {
        ESP_LOGW(TAG, "ts 不可信(%lld),丢弃这一帧", (long long)snap.ts_unix);
        free(payload);
        return;
    }

    esp_err_t r = dev->cfg.publish_fn(payload, dev->cfg.publish_user);
    /* 约束: publish_fn 是用户回调,不允许在其内部再调 profile_* 任何接口,
     * 否则 q_drain 顺序会乱(可能在补发中间插入新的 enqueue/drain) */
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "delta 发布成功 (%u 字节)", (unsigned)strlen(payload));
        /* 补发之前离线积压
         *
         * 注意 drain_per_tick 默认 8 + 当前这条 = 9 条/分钟 连发,
         * 弱网 + MQTT QoS=1 下可能压力较大;如未来出现 MQTT
         * 重发风暴,可将 cfg.drain_per_tick 调低到 3~5 */
        if (dev->cfg.spiffs && dev->queue_count > 0) {
            uint32_t sent = q_drain(dev, dev->cfg.drain_per_tick);
            if (sent > 0) {
                ESP_LOGI(TAG, "queue: 补发 %lu 条,剩余 %lu",
                         (unsigned long)sent, (unsigned long)dev->queue_count);
            }
        }
    } else if (r == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "publish 跳过(上游未就绪)");
        /* 上游未就绪 → 落盘 */
        if (dev->cfg.spiffs) {
            (void)q_enqueue(dev, snap.ts_unix, payload);
        }
    } else {
        dev->fail_count++;
        ESP_LOGW(TAG, "publish 失败: %s (fail_total=%lu)",
                 esp_err_to_name(r), (unsigned long)dev->fail_count);
        if (dev->cfg.spiffs) {
            (void)q_enqueue(dev, snap.ts_unix, payload);
        }
    }
    free(payload);
}

static char *profile_build_json(const profile_delta_t *d)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "ts",            (double)d->ts_unix);
    cJSON_AddNumberToObject(root, "win",           (double)d->window_s);
    cJSON_AddNumberToObject(root, "static",        (double)d->static_s);
    cJSON_AddNumberToObject(root, "walk_slow",     (double)d->walk_slow_s);
    cJSON_AddNumberToObject(root, "walk_fast",     (double)d->walk_fast_s);
    cJSON_AddNumberToObject(root, "run",           (double)d->run_s);
    cJSON_AddNumberToObject(root, "shake_or_fall", (double)d->shake_or_fall_s);
    cJSON_AddNumberToObject(root, "shake_n",       (double)d->shake_n);
    /* steps 负值 → 不上报(SNTP 未同步 / 主循环还未 setter) */
    if (d->steps_today >= 0) {
        cJSON_AddNumberToObject(root, "steps",     (double)d->steps_today);
    }
    cJSON_AddNumberToObject(root, "intensity_avg", (double)d->intensity_avg);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* ============================================================
 *  SPIFFS 离线队列(store-and-forward)
 *
 *  约定:
 *   - 一条 delta 一文件,文件名 = <base>/profile_q_<ts>.json
 *     ts 为 12 位十进制 unix 秒(2001~2286 区间均够,字典序 = 时间序)
 *   - 默认 200 条上限,超出按文件名(=ts)从最老开始删
 *   - publish 成功后顺序补发 drain_per_tick 条(默认 8)
 *   - 不持 dev->mtx,所有 q_* 仅在 publish_and_reset 路径上执行(单线程)
 * ============================================================ */

static int q_make_path(const profile_dev_t *d, int64_t ts, char *out, size_t cap)
{
    /* base_path/profile_q_NNNNNNNNNNNN.json
     * "profile_q_" (10) + 12 + ".json" (5) = 27 + base_path */
    return snprintf(out, cap, "%s/" PROFILE_Q_PREFIX "%012lld" PROFILE_Q_SUFFIX,
                    d->cfg.spiffs->base_path, (long long)ts);
}

/* 是否为我们的队列文件;返回 true 时 *ts_out 填入解析的 ts */
static bool q_match_name(const char *name, int64_t *ts_out)
{
    size_t plen = strlen(PROFILE_Q_PREFIX);
    size_t slen = strlen(PROFILE_Q_SUFFIX);
    size_t nlen = strlen(name);
    if (nlen < plen + 1 + slen) return false;
    if (memcmp(name, PROFILE_Q_PREFIX, plen) != 0) return false;
    if (memcmp(name + nlen - slen, PROFILE_Q_SUFFIX, slen) != 0) return false;
    /* 中间应当全是数字 */
    for (size_t i = plen; i < nlen - slen; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    if (ts_out) {
        *ts_out = strtoll(name + plen, NULL, 10);
    }
    return true;
}

static uint32_t q_count_files(const profile_dev_t *d)
{
    DIR *dir = opendir(d->cfg.spiffs->base_path);
    if (!dir) return 0;
    uint32_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (q_match_name(ent->d_name, NULL)) n++;
    }
    closedir(dir);
    return n;
}

/* 找最老一条(ts 最小);返回 0=找到,-1=空 */
static int q_scan_oldest(const profile_dev_t *d, char *out_path, size_t cap)
{
    DIR *dir = opendir(d->cfg.spiffs->base_path);
    if (!dir) return -1;
    int64_t best_ts = INT64_MAX;
    char    best_name[64] = {0};
    struct dirent *ent;
    int64_t ts;
    while ((ent = readdir(dir)) != NULL) {
        if (!q_match_name(ent->d_name, &ts)) continue;
        if (ts < best_ts) {
            best_ts = ts;
            strncpy(best_name, ent->d_name, sizeof(best_name) - 1);
            best_name[sizeof(best_name) - 1] = '\0';
        }
    }
    closedir(dir);
    if (best_name[0] == '\0') return -1;
    snprintf(out_path, cap, "%s/%s", d->cfg.spiffs->base_path, best_name);
    return 0;
}

/* 上限淘汰:循环删最老直到 count <= queue_max */
static void q_enforce_cap(profile_dev_t *d)
{
    while (d->queue_count > d->cfg.queue_max) {
        char path[96];
        if (q_scan_oldest(d, path, sizeof(path)) != 0) break;
        if (unlink(path) == 0) {
            d->queue_count--;
            ESP_LOGW(TAG, "queue: 容量满,删 %s", path);
        } else {
            ESP_LOGW(TAG, "queue: unlink 失败 %s", path);
            break;
        }
    }
}

static esp_err_t q_enqueue(profile_dev_t *d, int64_t ts, const char *json)
{
    if (!d->cfg.spiffs || !json) return ESP_ERR_INVALID_ARG;

    char path[96];
    q_make_path(d, ts, path, sizeof(path));

    /* 同名文件已存在 → 覆盖不加计数(避免 count 与实际文件数偏调) */
    struct stat st;
    bool already_exists = (stat(path, &st) == 0);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "queue: fopen 失败 %s", path);
        return ESP_FAIL;
    }
    size_t len = strlen(json);
    size_t w   = fwrite(json, 1, len, fp);
    fclose(fp);
    if (w != len) {
        ESP_LOGW(TAG, "queue: 写入不全 %u/%u", (unsigned)w, (unsigned)len);
        unlink(path);
        return ESP_FAIL;
    }
    if (!already_exists) d->queue_count++;
    /* 超上限 → 淘汰最老(可能就是刚写的相邻文件,极端情况) */
    q_enforce_cap(d);
    ESP_LOGI(TAG, "queue: 入队 ts=%lld (count=%lu%s)",
             (long long)ts, (unsigned long)d->queue_count,
             already_exists ? ", 覆盖" : "");
    return ESP_OK;
}

/* 顺序读最老文件、调 publish_fn、成功就 unlink;返回成功补发的条数 */
static uint32_t q_drain(profile_dev_t *d, uint32_t max_n)
{
    uint32_t sent = 0;
    for (uint32_t i = 0; i < max_n; i++) {
        char path[96];
        if (q_scan_oldest(d, path, sizeof(path)) != 0) break;

        /* 读文件 */
        FILE *fp = fopen(path, "rb");
        if (!fp) { ESP_LOGW(TAG, "queue: fopen %s 失败", path); break; }
        struct stat st;
        if (fstat(fileno(fp), &st) != 0 || st.st_size <= 0) {
            fclose(fp); unlink(path); d->queue_count--; continue;
        }
        char *buf = malloc((size_t)st.st_size + 1);
        if (!buf) { fclose(fp); break; }
        size_t rd = fread(buf, 1, (size_t)st.st_size, fp);
        fclose(fp);
        if (rd != (size_t)st.st_size) {
            free(buf);
            ESP_LOGW(TAG, "queue: 读取不全 %s", path);
            unlink(path); d->queue_count--;
            continue;
        }
        buf[rd] = '\0';

        esp_err_t r = d->cfg.publish_fn(buf, d->cfg.publish_user);
        free(buf);

        if (r == ESP_OK) {
            unlink(path);
            d->queue_count--;
            sent++;
        } else {
            /* 上游又掉了 / 失败 → 停止本轮,下次 publish 成功再来
             *
             * 已知限制(不修): 如果队首文件内容损坏 或 服务器对该 payload
             * 永远返回非 ESP_OK,这里每次都会 break 在同一条上,后面所有
             * 文件饥死。 MQTT 临时不可用(ESP_ERR_INVALID_STATE 等)是正常
             * 场景,应该 break;不响应区分错误类型是为了逻辑简单。
             * 如需兑底,可加单文件连续失败计数 N 次后 unlink 丢弃。
             * 现阶段: 真出现卡死需人工删 /spiffs/profile_q_*.json */
            ESP_LOGD(TAG, "queue: drain 中止 %s (%s)", path, esp_err_to_name(r));
            break;
        }
    }
    return sent;
}
