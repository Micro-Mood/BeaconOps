/**
 * @file sensor_task.c
 */
#include "sensor_task.h"
#include "config.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/atomic.h"

static const char *TAG = "sensor_task";

/* ---- sampling -------------------------------------------------------------- */
#define SAMPLE_HZ          50
#define SAMPLE_PERIOD_MS   (1000 / SAMPLE_HZ)
#define WINDOW_SAMPLES     SAMPLE_HZ          /* 1 s — for energy/state */
#define MG_TO_MS2          (9.80665f / 1000.0f)
#define MDPS_TO_DPS        (1.0f / 1000.0f)

/* ---- shake detection (§7.5) ----------------------------------------------- */
#define SHAKE_WIN_MS       1500
#define SHAKE_WIN_N        ((SAMPLE_HZ * SHAKE_WIN_MS) / 1000)  /* 75 */
#define SHAKE_THRESH_MS2   6.0f
#define SHAKE_MIN_CROSS    4
/* SHAKE_LOCKOUT_MS 由 config.h 提供;默认 2500ms,可在 config 调节 */
#ifndef SHAKE_LOCKOUT_MS
#define SHAKE_LOCKOUT_MS   2500
#endif

/* ---- motion ping (for backlight) ------------------------------------------ */
#define MOTION_THRESH_MS2  1.5f
#define MOTION_MIN_GAP_MS  100   /* throttle to ≤ 10 Hz */

/* ---- gravity HPF ----------------------------------------------------------- */
#define HPF_ALPHA          0.995f  /* low-pass for gravity estimate; cutoff ~0.04 Hz at 50 Hz */

static struct {
    sensor_task_cfg_t cfg;
    bool              inited;

    /* gravity estimate (m/s²) */
    float gx_g, gy_g, gz_g;

    /* 1 s ring of magnitude (m/s², gravity-removed) for behavior */
    float bh_ring[WINDOW_SAMPLES];
    int   bh_pos;
    int   bh_count;
    int   bh_tick_acc;   /* counts samples until next 1 Hz update */

    /* 1.5 s ring of signed primary-axis a for shake */
    float sh_ring[SHAKE_WIN_N];
    int   sh_pos;
    int   sh_count;
    uint32_t sh_lockout_until_ms;

    /* motion throttle */
    uint32_t last_motion_ms;

    /* atomic-ish state mirror */
    volatile int last_state;
} S;

/* ---------------------------------------------------------------------------- */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

/** Pick the axis with the largest linear-accel variance over the shake window.
 *  Cheap heuristic: just use Z because the device is worn upright; subtract
 *  gravity via HPF. Spec §7.5 says "z 轴或合成轴". */
static int count_zero_crossings_above_thresh(void)
{
    int crossings = 0;
    int last_sign = 0;  /* 0 = unknown, +1, -1 */
    int n = S.sh_count;
    int idx = (S.sh_pos - n + SHAKE_WIN_N) % SHAKE_WIN_N;
    for (int i = 0; i < n; ++i) {
        float v = S.sh_ring[idx];
        if (++idx >= SHAKE_WIN_N) idx = 0;
        if (fabsf(v) < SHAKE_THRESH_MS2) continue;  /* below trigger band */
        int sign = (v > 0) ? 1 : -1;
        if (last_sign != 0 && sign != last_sign) crossings++;
        last_sign = sign;
    }
    return crossings;
}

static behavior_state_e classify_energy(float energy, int *intensity_out)
{
    /* energy in (m/s²)² ; thresholds from §7.2 */
    behavior_state_e st;
    if      (energy < 0.05f) st = BEHAVIOR_STATIC;
    else if (energy < 0.5f)  st = BEHAVIOR_WALK_SLOW;
    else if (energy < 2.0f)  st = BEHAVIOR_WALK_FAST;
    else if (energy < 5.0f)  st = BEHAVIOR_RUN;
    else                     st = BEHAVIOR_SHAKE_OR_FALL;
    if (intensity_out) {
        float v = 2.0f * log10f(energy + 1.0f) * 5.0f;
        int   i = (int)lroundf(v);
        if (i < 1) i = 1; else if (i > 10) i = 10;
        *intensity_out = i;
    }
    return st;
}

static void process_sample(const sensor_data_t *sd)
{
    /* mg → m/s² */
    float ax = sd->accel_x * MG_TO_MS2;
    float ay = sd->accel_y * MG_TO_MS2;
    float az = sd->accel_z * MG_TO_MS2;

    /* update gravity estimates (LPF) */
    S.gx_g = HPF_ALPHA * S.gx_g + (1.0f - HPF_ALPHA) * ax;
    S.gy_g = HPF_ALPHA * S.gy_g + (1.0f - HPF_ALPHA) * ay;
    S.gz_g = HPF_ALPHA * S.gz_g + (1.0f - HPF_ALPHA) * az;

    /* linear acceleration */
    float lx = ax - S.gx_g;
    float ly = ay - S.gy_g;
    float lz = az - S.gz_g;
    float lmag = sqrtf(lx*lx + ly*ly + lz*lz);

    /* ---- behavior 1 s ring ---- */
    S.bh_ring[S.bh_pos] = lmag;
    if (++S.bh_pos >= WINDOW_SAMPLES) S.bh_pos = 0;
    if (S.bh_count < WINDOW_SAMPLES) S.bh_count++;

    /* ---- shake 1.5 s ring (use lz primary axis) ---- */
    S.sh_ring[S.sh_pos] = lz;
    if (++S.sh_pos >= SHAKE_WIN_N) S.sh_pos = 0;
    if (S.sh_count < SHAKE_WIN_N) S.sh_count++;

    uint32_t t = now_ms();

    /* ---- motion ping ---- */
    if (lmag > MOTION_THRESH_MS2 &&
        (t - S.last_motion_ms) >= MOTION_MIN_GAP_MS) {
        S.last_motion_ms = t;
        if (S.cfg.on_motion) S.cfg.on_motion(S.cfg.user);
    }

    /* ---- shake confirm ---- */
    if (S.sh_count >= SHAKE_WIN_N && t >= S.sh_lockout_until_ms) {
        if (count_zero_crossings_above_thresh() >= SHAKE_MIN_CROSS) {
            ESP_LOGI(TAG, "shake confirmed");
            S.sh_lockout_until_ms = t + SHAKE_LOCKOUT_MS;
            if (S.cfg.on_shake) S.cfg.on_shake(S.cfg.user);
        }
    }

    /* ---- 1 Hz behavior state ---- */
    if (++S.bh_tick_acc >= SAMPLE_HZ) {
        S.bh_tick_acc = 0;
        if (S.bh_count > 0) {
            float sum_sq = 0.0f;
            for (int i = 0; i < S.bh_count; ++i) {
                sum_sq += S.bh_ring[i] * S.bh_ring[i];
            }
            float energy = sum_sq / (float)S.bh_count;
            int intensity = 1;
            behavior_state_e st = classify_energy(energy, &intensity);
            S.last_state = (int)st;
            if (S.cfg.on_behavior) S.cfg.on_behavior(st, intensity, S.cfg.user);
        }
    }
}

static void sensor_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    sensor_data_t sd;
    while (1) {
        if (S.cfg.dev && sensor_read_data(S.cfg.dev, &sd) == ESP_OK) {
            process_sample(&sd);
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

/* ---------------------------------------------------------------------------- */

esp_err_t sensor_task_start(const sensor_task_cfg_t *cfg)
{
    if (S.inited) return ESP_OK;
    if (!cfg || !cfg->dev) return ESP_ERR_INVALID_ARG;

    memset(&S, 0, sizeof(S));
    S.cfg = *cfg;
    /* seed gravity at +9.8 on z (typical wear orientation). */
    S.gz_g = 9.80665f;
    S.last_state = BEHAVIOR_STATIC;

    BaseType_t r = xTaskCreate(sensor_task, "sensor", 2560,
                               NULL, tskIDLE_PRIORITY + 5, NULL);
    if (r != pdPASS) return ESP_FAIL;
    S.inited = true;
    ESP_LOGI(TAG, "started @ %d Hz (shake win=%d samples)", SAMPLE_HZ, SHAKE_WIN_N);
    return ESP_OK;
}

behavior_state_e sensor_task_get_state(void)
{
    return (behavior_state_e)S.last_state;
}
