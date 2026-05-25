/**
 * @file sensor_task.h
 * @brief 50 Hz IMU sampling task with shake detection + motion-energy classifier.
 *
 * Implements 02-客户端固件规格.md §7.1–§7.5:
 *   - 50 Hz polling of LSM6DS3TR-C
 *   - Linear-acceleration HPF (gravity removal)
 *   - 1 Hz energy-based behavior state machine
 *   - 1.5 s sliding window shake detection (zero-crossing of |a| > 6 m/s²,
 *     ≥ 4 crossings → confirmed; 2 s lockout)
 *
 * Callbacks fire from the internal task (priority idle+5). They must be
 * non-blocking; for ack handoff use a flag/queue (e.g. msg_svc_on_shake).
 */
#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include <stdbool.h>
#include "esp_err.h"
#include "sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BEHAVIOR_STATIC      = 0,
    BEHAVIOR_WALK_SLOW   = 1,
    BEHAVIOR_WALK_FAST   = 2,
    BEHAVIOR_RUN         = 3,
    BEHAVIOR_SHAKE_OR_FALL = 4,
} behavior_state_e;

/** Per-sample motion ping (linear |a| > MOTION_THRESH, default 1.5 m/s²).
 *  Used for backlight wake. Throttled internally to ≤ 10 Hz. */
typedef void (*sensor_motion_fn)(void *user);

/** Confirmed shake gesture per §7.5. Honors the 2 s lockout. */
typedef void (*sensor_shake_fn)(void *user);

/** 1 Hz behavior state update. May be NULL if not needed. */
typedef void (*sensor_behavior_fn)(behavior_state_e st, int intensity_1_10, void *user);

typedef struct {
    sensor_dev_t       *dev;          ///< already-initialised LSM6DS3TR-C handle
    sensor_motion_fn    on_motion;
    sensor_shake_fn     on_shake;
    sensor_behavior_fn  on_behavior;  ///< optional
    void               *user;
} sensor_task_cfg_t;

/** Spawn the task. Idempotent. */
esp_err_t sensor_task_start(const sensor_task_cfg_t *cfg);

/** Read latest cached behavior state (atomic snapshot). */
behavior_state_e sensor_task_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_TASK_H */
