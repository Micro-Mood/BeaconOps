# Sensor

A thin wrapper around the **LSM6DS3TR-C** 6-axis IMU. It only does three things: bring the chip up, read raw data, and set up the chip's built-in interrupt features.

It does **not** decide what counts as a "shake" or a "pickup" — that kind of business logic lives in `sensor_task` and `main/main.cpp`. Conversely, if you want to lift this driver into another project, you will not be dragging any BeaconOps-specific behavior along with it.

## Documentation Map

- Parent components overview: `../README_EN.md`
- Chinese version: `README.md`

---

## Dependencies

- `I2C/i2c_bus_manager.h`: shared I2C bus access
- `lsm6ds3trc.h`: register-level chip driver
- `config.h`: I2C pins and frequency are managed from the global configuration

---

## Capabilities

### Core Capabilities

- `lsm6ds3tr_c_esp32_init`: initialize the device and bind it to the I2C bus
- `lsm6ds3tr_c_check_connection`: check whether the chip is reachable
- `lsm6ds3tr_c_read_data`: read raw acceleration, gyro, and temperature data
- `lsm6ds3tr_c_get_ctx`: get the low-level register access context

### Sampling Configuration

- `lsm6ds3tr_c_config_accel`: configure accelerometer sample rate and full scale
- `lsm6ds3tr_c_config_gyro`: configure gyroscope sample rate and full scale

### Interrupt and Event Support

- `lsm6ds3tr_c_config_interrupts`: configure interrupt pins and GPIO behavior
- `lsm6ds3tr_c_register_int_callback`: register an interrupt callback
- `lsm6ds3tr_c_enable_interrupt`: enable or disable a specific interrupt source
- `lsm6ds3tr_c_read_int_status`: read the current interrupt status

### Built-In Detection Features

- `lsm6ds3tr_c_config_tap_detection`: single / double tap detection
- `lsm6ds3tr_c_config_free_fall_detection`: free-fall detection
- `lsm6ds3tr_c_config_step_detection`: step detection
- `lsm6ds3tr_c_config_wake_up_detection`: wake-up detection
- `lsm6ds3tr_c_config_inactivity_detection`: inactivity detection
- `lsm6ds3tr_c_config_6d_orientation`: 6D orientation detection

---

## Role Inside BeaconOps

In this project, `sensor/` is only responsible for providing raw IMU data and low-level interrupt configuration:

1. `app_sensor_init()` creates the sensor handle and completes the base initialization.
2. `sensor_task` keeps sampling at runtime.
3. `sensor_task` converts raw data into motion / shake / behavior callbacks and forwards them to `msg` and `profile`.

So if you want to change:

- **I2C wiring, full scale, sample rate**: look at this component and `config.h`
- **shake thresholds, behavior throttling, business-level reactions**: look at `sensor_task` and `main/main.cpp`

---

## Notes

- This component is part of the hardware-driver layer. It does not handle UI, message reporting, or prompt tones.
- Project-specific business semantics should remain in upper-layer tasks rather than being pushed into the driver layer.
- If you replace the IMU model, try to preserve the external interface of this component so the higher-level assembly code changes as little as possible.