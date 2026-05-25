/**
 * @file notify.c
 * @brief 手写合成提示音组件实现
 */

#include "notify.h"
#include "config.h"

#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "notify";

/* ---- 默认值 ------------------------------------------------------------ */
#define NOTIFY_DEF_AMP              8000
#define NOTIFY_DEF_SCRATCH_MS       50
#define NOTIFY_DEF_QUEUE_DEPTH      8
#define NOTIFY_DEF_TASK_PRIO        (tskIDLE_PRIORITY + 2)
#define NOTIFY_DEF_TASK_STACK       3072
#define NOTIFY_DEF_LOCK_TIMEOUT_MS  1000

#define NOTIFY_KIND_CONFIRM         0x80
#define NOTIFY_PRE_SILENCE_MS       12
#define NOTIFY_POST_SILENCE_MS      100
#define NOTIFY_RAMP_MS              8

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    uint16_t freq_hz;
    uint16_t duration_ms;
    uint16_t gap_ms;
    uint8_t  volume_pct;
} notify_tone_step_t;

typedef struct {
    const notify_tone_step_t *steps;
    size_t count;
    const char *name;
} notify_pattern_t;

static const notify_tone_step_t s_info_steps[] = {
    { 1568, 115, 0, 45 },
};

static const notify_tone_step_t s_notice_steps[] = {
    { 1175, 65, 12, 48 },
    { 1568, 65, 12, 48 },
    { 1976, 85, 0,  48 },
};

static const notify_tone_step_t s_warn_steps[] = {
    { 1397, 85, 26, 55 },
    { 1760, 85, 42, 55 },
    { 1397, 85, 26, 55 },
    { 1760, 105, 0, 55 },
};

static const notify_tone_step_t s_emerg_steps[] = {
    { 1568, 75, 18, 62 },
    { 2093, 75, 18, 62 },
    { 1568, 75, 18, 62 },
    { 2093, 75, 18, 62 },
    { 1568, 75, 18, 62 },
    { 2093, 105, 0, 62 },
};

static const notify_tone_step_t s_confirm_steps[] = {
    { 1175, 50, 10, 48 },
    { 1568, 50, 10, 48 },
    { 1976, 70, 0,  48 },
};

static const notify_pattern_t s_level_patterns[NOTIFY_LV_MAX] = {
    [NOTIFY_LV_INFO]   = { s_info_steps,   ARRAY_SIZE(s_info_steps),   "info"   },
    [NOTIFY_LV_NOTICE] = { s_notice_steps, ARRAY_SIZE(s_notice_steps), "notice" },
    [NOTIFY_LV_WARN]   = { s_warn_steps,   ARRAY_SIZE(s_warn_steps),   "warn"   },
    [NOTIFY_LV_EMERG]  = { s_emerg_steps,  ARRAY_SIZE(s_emerg_steps),  "emerg"  },
};

static const notify_pattern_t s_confirm_pattern = {
    s_confirm_steps, ARRAY_SIZE(s_confirm_steps), "confirm"
};

/* ---- 句柄定义 ---------------------------------------------------------- */
struct notify_dev_s {
    notify_config_t   cfg;
    QueueHandle_t     queue;
    TaskHandle_t      task;
    int16_t          *scratch;        ///< 堆分配
    size_t            scratch_samples;
    bool              session_lock_max;
    volatile bool     stop;
    bool              initialized;
};

/* ---- 前向声明 ---------------------------------------------------------- */
static void      notify_worker_task(void *arg);
static void      notify_render_pattern(notify_dev_t *dev, const notify_pattern_t *pattern);
static esp_err_t notify_render_tone(notify_dev_t *dev, const notify_tone_step_t *step);
static void      notify_render_silence(notify_dev_t *dev, uint32_t ms);

/* ============================================================
 *  公共 API
 * ============================================================ */

esp_err_t notify_init(notify_dev_t **dev, const notify_config_t *config)
{
    if (!dev || !config || !config->audio) return ESP_ERR_INVALID_ARG;

    notify_dev_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;

    d->cfg = *config;
    if (d->cfg.sample_rate == 0)             d->cfg.sample_rate = AUDIO_SAMPLE_RATE;
    if (d->cfg.amplitude   == 0)             d->cfg.amplitude   = NOTIFY_DEF_AMP;
    if (d->cfg.scratch_ms  == 0)             d->cfg.scratch_ms  = NOTIFY_DEF_SCRATCH_MS;
    if (d->cfg.queue_depth == 0)             d->cfg.queue_depth = NOTIFY_DEF_QUEUE_DEPTH;
    if (d->cfg.task_prio   == 0)             d->cfg.task_prio   = NOTIFY_DEF_TASK_PRIO;
    if (d->cfg.task_stack  == 0)             d->cfg.task_stack  = NOTIFY_DEF_TASK_STACK;
    if (d->cfg.session_lock_timeout_ms == 0) d->cfg.session_lock_timeout_ms = NOTIFY_DEF_LOCK_TIMEOUT_MS;

    d->session_lock_max = (d->cfg.session_lock_timeout_ms == UINT32_MAX);

    /* scratch 缓冲 */
    d->scratch_samples = (size_t)d->cfg.sample_rate * d->cfg.scratch_ms / 1000;
    if (d->scratch_samples == 0) d->scratch_samples = 64;
    d->scratch = malloc(d->scratch_samples * sizeof(int16_t));
    if (!d->scratch) goto fail;

    /* 队列 + 任务 */
    d->queue = xQueueCreate(d->cfg.queue_depth, sizeof(uint8_t));
    if (!d->queue) goto fail;

    BaseType_t r = xTaskCreate(notify_worker_task, "notify",
                               d->cfg.task_stack, d,
                               d->cfg.task_prio, &d->task);
    if (r != pdPASS) goto fail;

    d->initialized = true;
    *dev = d;
    ESP_LOGI(TAG, "提示音初始化完成 (sr=%lu, amp=%u, scratch=%zu samples)",
             (unsigned long)d->cfg.sample_rate,
             (unsigned)d->cfg.amplitude,
             d->scratch_samples);
    return ESP_OK;

fail:
    if (d->task)    vTaskDelete(d->task);
    if (d->queue)   vQueueDelete(d->queue);
    if (d->scratch) free(d->scratch);
    free(d);
    *dev = NULL;
    return ESP_FAIL;
}

esp_err_t notify_deinit(notify_dev_t **dev)
{
    if (!dev || !*dev) return ESP_ERR_INVALID_ARG;
    notify_dev_t *d = *dev;
    if (!d->initialized) return ESP_ERR_INVALID_STATE;

    /* 让 worker 自行退出 */
    d->stop = true;
    if (d->task) {
        uint8_t sentinel = 0xFF;
        if (d->queue) xQueueSend(d->queue, &sentinel, 0);
        for (int i = 0; i < 30 && eTaskGetState(d->task) != eDeleted; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (eTaskGetState(d->task) != eDeleted) {
            vTaskDelete(d->task);
        }
        d->task = NULL;
    }
    if (d->queue)   vQueueDelete(d->queue);
    if (d->scratch) free(d->scratch);

    d->initialized = false;
    free(d);
    *dev = NULL;
    ESP_LOGI(TAG, "提示音反初始化完成");
    return ESP_OK;
}

esp_err_t notify_play(notify_dev_t *dev, notify_level_e level)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    if (level >= NOTIFY_LV_MAX)    return ESP_ERR_INVALID_ARG;

    uint8_t lv = (uint8_t)level;
    if (xQueueSend(dev->queue, &lv, 0) != pdTRUE) {
        ESP_LOGW(TAG, "队列已满,丢弃 lv=%d", (int)level);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t notify_play_confirm(notify_dev_t *dev)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    uint8_t kind = NOTIFY_KIND_CONFIRM;
    if (xQueueSend(dev->queue, &kind, 0) != pdTRUE) {
        ESP_LOGW(TAG, "队列已满,丢弃 confirm");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ============================================================
 *  内部 — 渲染
 * ============================================================ */

static const int16_t s_sine_table[256] = {
        0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
     6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
    12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
    18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
    23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
    27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
    30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
    32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
    32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
    32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
    30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
    27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
    23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
    18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
    12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
     6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
        0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
    -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
   -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
   -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
   -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
   -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
   -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
   -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
   -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
   -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
   -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
   -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
   -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
   -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
   -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
    -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

static int16_t notify_sine_wave(uint32_t phase)
{
    uint8_t idx = (uint8_t)(phase >> 24);
    uint8_t frac = (uint8_t)(phase >> 16);
    int32_t a = s_sine_table[idx];
    int32_t b = s_sine_table[(uint8_t)(idx + 1)];
    return (int16_t)(a + (((b - a) * frac) >> 8));
}

static uint32_t notify_envelope(uint32_t index, uint32_t total, uint32_t ramp)
{
    if (ramp == 0) return 32768;
    if (index < ramp) return (uint32_t)((uint64_t)index * 32768 / ramp);

    uint32_t remain = total - index;
    if (remain < ramp) return (uint32_t)((uint64_t)remain * 32768 / ramp);
    return 32768;
}

static void notify_render_silence(notify_dev_t *dev, uint32_t ms)
{
    if (ms == 0) return;

    esp_err_t ret = audio_play_silence(dev->cfg.audio, ms);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "silence failed: %s", esp_err_to_name(ret));
    }
}

static esp_err_t notify_render_tone(notify_dev_t *dev, const notify_tone_step_t *step)
{
    if (!step || step->freq_hz == 0 || step->duration_ms == 0) return ESP_OK;

    uint32_t sample_rate = (uint32_t)dev->cfg.sample_rate;
    uint32_t total = (uint32_t)(((uint64_t)sample_rate * step->duration_ms + 999) / 1000);
    uint32_t ramp = sample_rate * NOTIFY_RAMP_MS / 1000;
    uint32_t amplitude = (uint32_t)dev->cfg.amplitude * step->volume_pct / 100;
    uint32_t phase_step = (uint32_t)(((uint64_t)step->freq_hz << 32) / sample_rate);

    if (total == 0) return ESP_OK;
    if (ramp * 2 > total) ramp = total / 2;
    if (amplitude > 50000) amplitude = 50000;

    uint32_t offset = 0;
    while (offset < total) {
        size_t chunk = total - offset;
        if (chunk > dev->scratch_samples) chunk = dev->scratch_samples;

        for (size_t i = 0; i < chunk; ++i) {
            uint32_t index = offset + (uint32_t)i;
            uint32_t phase = (uint32_t)((uint64_t)index * phase_step);
            int32_t wave = notify_sine_wave(phase);
            int32_t env = (int32_t)notify_envelope(index, total, ramp);
            int32_t value = wave * (int32_t)amplitude / 32768;
            value = value * env / 32768;
            dev->scratch[i] = (int16_t)value;
        }

        size_t bytes_written = 0;
        esp_err_t ret = audio_play(dev->cfg.audio, dev->scratch,
                                   chunk * sizeof(int16_t), &bytes_written);
        if (ret != ESP_OK) return ret;

        size_t samples_written = bytes_written / sizeof(int16_t);
        if (samples_written == 0) return ESP_FAIL;
        offset += (uint32_t)samples_written;
    }

    return ESP_OK;
}

static void notify_render_pattern(notify_dev_t *dev, const notify_pattern_t *pattern)
{
    if (!pattern || !pattern->steps || pattern->count == 0) return;

    for (size_t i = 0; i < pattern->count; ++i) {
        esp_err_t ret = notify_render_tone(dev, &pattern->steps[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "%s tone failed: %s", pattern->name, esp_err_to_name(ret));
            return;
        }
        notify_render_silence(dev, pattern->steps[i].gap_ms);
    }
}

/* ============================================================
 *  内部 — worker task
 * ============================================================ */

static void notify_worker_task(void *arg)
{
    notify_dev_t *dev = (notify_dev_t *)arg;

    while (!dev->stop) {
        uint8_t lv = 0;
        if (xQueueReceive(dev->queue, &lv, portMAX_DELAY) != pdTRUE) continue;
        if (dev->stop || lv == 0xFF) break;     /* sentinel */
        const notify_pattern_t *pattern = NULL;
        if (lv == NOTIFY_KIND_CONFIRM) {
            pattern = &s_confirm_pattern;
        } else if (lv < NOTIFY_LV_MAX) {
            pattern = &s_level_patterns[lv];
        } else {
            continue;
        }
        if (!pattern->steps || pattern->count == 0) continue;

        /* 占用 audio 会话锁;失败则跳过本次,不阻塞 */
        uint32_t to_ms = dev->session_lock_max ? UINT32_MAX
                                               : dev->cfg.session_lock_timeout_ms;
        esp_err_t r = audio_session_acquire(dev->cfg.audio, to_ms);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "session_acquire 失败,跳过 lv=%d (%s)",
                     (int)lv, esp_err_to_name(r));
            continue;
        }

        notify_render_silence(dev, NOTIFY_PRE_SILENCE_MS);
        notify_render_pattern(dev, pattern);
        notify_render_silence(dev, NOTIFY_POST_SILENCE_MS);
        audio_session_release(dev->cfg.audio);
    }
    vTaskDelete(NULL);
}
