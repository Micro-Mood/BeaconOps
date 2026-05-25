/**
 * @file backlight_mgr.c
 */
#include "backlight_mgr.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"

static const char *TAG = "backlight";

#define LEDC_TIMER_BIT      LEDC_TIMER_8_BIT
#define LEDC_DUTY_MAX       255
#define LEDC_TIMER_NUM      LEDC_TIMER_0
#define LEDC_CH_NUM         LEDC_CHANNEL_0
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define RAMP_STEP_MS        10

static struct {
    backlight_mgr_cfg_t cfg;
    bool       inited;

    /* state machine */
    int        target_duty;          /* desired final duty (0 or MAX) */
    int        current_duty;
    uint32_t   last_kick_ms;         /* last motion-or-force timestamp */
    uint32_t   wake_until_ms;        /* §7.7 grace window end */
    bool       force_on;

    /* PM lock: 背光亮起期间持 NO_LIGHT_SLEEP 锁
     *   - 亮屏时禁止进 light sleep → PWM/SPI/LCD 状态稳定,不闪
     *   - 灭屏后释锁 → 允许 auto light sleep,平均电流限译 ~10mA */
    esp_pm_lock_handle_t pm_lock;
    bool       pm_lock_held;
} S;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static void pm_lock_acquire(void)
{
    if (S.pm_lock && !S.pm_lock_held) {
        if (esp_pm_lock_acquire(S.pm_lock) == ESP_OK) {
            S.pm_lock_held = true;
        }
    }
}

static void pm_lock_release(void)
{
    if (S.pm_lock && S.pm_lock_held) {
        if (esp_pm_lock_release(S.pm_lock) == ESP_OK) {
            S.pm_lock_held = false;
        }
    }
}

static void apply_duty(int duty)
{
    if (duty < 0)            duty = 0;
    if (duty > LEDC_DUTY_MAX) duty = LEDC_DUTY_MAX;
    ledc_set_duty(LEDC_MODE, LEDC_CH_NUM, (uint32_t)duty);
    ledc_update_duty(LEDC_MODE, LEDC_CH_NUM);
    S.current_duty = duty;
}

/** Cubic ease-out: f(t) = 1 - (1-t)^3, t in [0,1]. */
static float ease_out_cubic(float t)
{
    if (t < 0) t = 0; else if (t > 1) t = 1;
    float u = 1.0f - t;
    return 1.0f - (u * u * u);
}

/** Blocking ramp from current_duty to target over @p ramp_ms. */
static void ramp_to(int target, uint32_t ramp_ms)
{
    int start = S.current_duty;
    if (start == target || ramp_ms == 0) { apply_duty(target); return; }
    uint32_t t0 = now_ms();
    while (1) {
        uint32_t dt = now_ms() - t0;
        if (dt >= ramp_ms) break;
        float t = (float)dt / (float)ramp_ms;
        float e = ease_out_cubic(t);
        int d  = start + (int)lroundf((float)(target - start) * e);
        apply_duty(d);
        vTaskDelay(pdMS_TO_TICKS(RAMP_STEP_MS));
    }
    apply_duty(target);
    S.target_duty = target;
}

/* ---------------------------------------------------------------------------- */

void backlight_mgr_kick_motion(void)
{
    if (!S.inited) return;
    uint32_t t = now_ms();
    S.last_kick_ms = t;
    if (S.target_duty == 0) {
        /* dark → wake. Set grace BEFORE ramping. */
        S.wake_until_ms = t + S.cfg.wake_grace_ms;
        S.target_duty   = LEDC_DUTY_MAX;
        pm_lock_acquire();      /* 亮屏期间禁 light sleep */
        ESP_LOGI(TAG, "wake (grace %ums)", (unsigned)S.cfg.wake_grace_ms);
    }
}

void backlight_mgr_force_on(void)
{
    if (!S.inited) return;
    S.force_on = true;
    S.last_kick_ms = now_ms();
    if (S.target_duty == 0) {
        S.target_duty = LEDC_DUTY_MAX;
    }
    pm_lock_acquire();          /* force_on 也要持锁 */
}

bool backlight_mgr_in_wake_grace(void)
{
    if (!S.inited) return false;
    return now_ms() < S.wake_until_ms;
}

bool backlight_mgr_is_on(void) { return S.inited && S.current_duty > 0; }

/* ---------------------------------------------------------------------------- */

static void bl_task(void *arg)
{
    (void)arg;
    while (1) {
        if (S.target_duty != S.current_duty) {
            int target = S.target_duty;
            uint32_t ramp = (target > S.current_duty) ? S.cfg.ramp_ms
                                                       : S.cfg.ramp_ms;
            ramp_to(target, ramp);
            /* 灭屏动画走完 → 释锁,让系统进 auto light sleep */
            if (S.current_duty == 0) {
                pm_lock_release();
            }
        } else if (!S.force_on && S.target_duty > 0) {
            /* Idle check. */
            uint32_t since = now_ms() - S.last_kick_ms;
            if (since >= S.cfg.idle_off_ms) {
                ESP_LOGI(TAG, "idle %ums → off", (unsigned)since);
                S.target_duty = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t backlight_mgr_init(const backlight_mgr_cfg_t *cfg)
{
    if (S.inited) return ESP_OK;
    if (!cfg || cfg->pin == GPIO_NUM_NC) return ESP_ERR_INVALID_ARG;

    memset(&S, 0, sizeof(S));
    S.cfg = *cfg;
    if (S.cfg.pwm_freq_hz   == 0) S.cfg.pwm_freq_hz   = 5000;
    if (S.cfg.idle_off_ms   == 0) S.cfg.idle_off_ms   = 10000;
    if (S.cfg.ramp_ms       == 0) S.cfg.ramp_ms       = 300;
    if (S.cfg.wake_grace_ms == 0) S.cfg.wake_grace_ms = 1000;

    ledc_timer_config_t tc = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER_NUM,
        .duty_resolution = LEDC_TIMER_BIT,
        .freq_hz         = S.cfg.pwm_freq_hz,
        /* RC_FAST(C3 ≈ 17.5MHz)在 light sleep 期间继续走,APB 不会停 PWM。
         * 不要用 LEDC_AUTO_CLK:它选 APB(80MHz),light sleep 时 APB 关闭
         * → PWM 输出冻结在瞬时电平 → 背光肉眼可见闪烁。 */
        .clk_cfg         = LEDC_USE_RC_FAST_CLK,
    };
    esp_err_t r = ledc_timer_config(&tc);
    if (r != ESP_OK) return r;

    ledc_channel_config_t cc = {
        .gpio_num   = S.cfg.pin,
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CH_NUM,
        .timer_sel  = LEDC_TIMER_NUM,
        .duty       = LEDC_DUTY_MAX,
        .hpoint     = 0,
    };
    r = ledc_channel_config(&cc);
    if (r != ESP_OK) return r;

    S.current_duty   = LEDC_DUTY_MAX;
    S.target_duty    = LEDC_DUTY_MAX;
    S.last_kick_ms   = now_ms();
    S.wake_until_ms  = 0;
    S.force_on       = false;

    /* 创建 NO_LIGHT_SLEEP 锁;初始亮屏 → 立刻持锁 */
    esp_err_t lr = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP,
                                      0, "backlight", &S.pm_lock);
    if (lr != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_lock_create failed: %s", esp_err_to_name(lr));
        S.pm_lock = NULL;
    }
    pm_lock_acquire();

    BaseType_t br = xTaskCreate(bl_task, "backlight", 2048,
                                NULL, tskIDLE_PRIORITY + 1, NULL);
    if (br != pdPASS) return ESP_FAIL;

    S.inited = true;
    ESP_LOGI(TAG, "ready (pin=%d, idle_off=%ums, ramp=%ums, grace=%ums)",
             S.cfg.pin,
             (unsigned)S.cfg.idle_off_ms,
             (unsigned)S.cfg.ramp_ms,
             (unsigned)S.cfg.wake_grace_ms);
    return ESP_OK;
}
