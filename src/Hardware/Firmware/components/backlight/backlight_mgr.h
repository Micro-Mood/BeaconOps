/**
 * @file backlight_mgr.h
 * @brief PWM backlight controller with idle-off, motion-wake, EMERG-force-on,
 *        and the §7.7 1 s wake_grace window.
 *
 * Owns the LCD backlight GPIO via LEDC (overrides whatever the panel driver
 * configured during init — the pin is just a digital output otherwise).
 *
 * Lifecycle:
 *   backlight_mgr_init();                 // start at 100 %, idle timer ticking
 *   backlight_mgr_kick_motion();          // call from sensor_task on_motion
 *   backlight_mgr_force_on();             // call when EMERG arrives
 *   bool s = backlight_mgr_in_wake_grace();// shake handler checks this
 */
#ifndef BACKLIGHT_MGR_H
#define BACKLIGHT_MGR_H

#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t pin;
    uint32_t   pwm_freq_hz;     ///< 0 → 5000
    uint32_t   idle_off_ms;     ///< 0 → 10000
    uint32_t   ramp_ms;         ///< 0 → 300
    uint32_t   wake_grace_ms;   ///< 0 → 1000
} backlight_mgr_cfg_t;

esp_err_t backlight_mgr_init(const backlight_mgr_cfg_t *cfg);

/** Defer auto-off; if currently dark, ramp back to full. */
void backlight_mgr_kick_motion(void);

/** Override idle logic — keeps the screen on until next motion event clears it. */
void backlight_mgr_force_on(void);

/** True during the post-wake grace window. */
bool backlight_mgr_in_wake_grace(void);

/** Current backlight state for diagnostics. */
bool backlight_mgr_is_on(void);

#ifdef __cplusplus
}
#endif

#endif /* BACKLIGHT_MGR_H */
