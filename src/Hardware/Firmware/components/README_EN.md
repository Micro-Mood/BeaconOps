# Components

Almost all of the BeaconOps firmware logic lives in `components/`. `main/main.cpp` is basically a wiring layer — it does hardware init, creates handles, and hooks up callbacks. The parts you actually tweak, reuse, or read to understand what the device is doing all live here. If you want to know what this firmware actually *does*, start here, not in `main`.

## Documentation Map

- Parent firmware overview: `../README_EN.md`
- Chinese version: `README.md`
- Sensor component: `sensor/README_EN.md`
- Sensor Chinese version: `sensor/README.md`

---

## Layered Structure

```text
components/
├── config/         Global configuration: pins, MQTT, batch credentials, Wi-Fi, SNTP
├── I2C/            Shared I2C bus manager
│
├── Hardware drivers
│   ├── sensor/     LSM6DS3TR-C IMU driver
│   ├── battery/    CW2017 fuel gauge
│   ├── audio/      MAX98357 I2S audio output
│   ├── backlight/  PWM backlight control
│   └── display/    ST7789 + LVGL 9.3 display stack
│
├── Local services
│   ├── fs/         NVS / SPIFFS wrappers
│   ├── settings/   Persistent settings
│   ├── error/      Error recording and log persistence
│   ├── decode/     PNG / JPG image decoding
│   └── certs/      TLS root certificate embedding
│
├── Protocol and networking
│   ├── identity/   device_id and HMAC password derivation
│   ├── net/        MQTT client and topic I/O
│   ├── msg/        Downlink parsing and deduplication
│   ├── tx/         ACK persistence and retry
│   ├── event/      Anomaly / event reporting
│   └── health/     Periodic health reporting
│
└── Application outputs
    ├── profile/    Behavior aggregation and profile_delta publishing
    ├── notify/     Prompt tone synthesis and playback scheduling
    └── ui/         Screen UI, message cards, and status display
```

---

## Assembly Relationship

At startup, responsibilities are divided as follows:

1. `main/main.cpp` synchronously initializes GPIO, I2C, IMU, display, backlight, NVS, SPIFFS, audio, battery gauge, and other base services.
2. `main/main.cpp` creates the `error`, `notify`, `ui`, and `wifi` handles and wires components together through callbacks.
3. `net_bringup_task` brings up `wifi -> sntp -> identity/hmac -> mqtt -> tx/msg/profile` in sequence in the background.
4. `sensor_task` runs independently, converts IMU samples into shake, motion, and behavior events, then forwards them to `msg` and `profile`.

So in practice:

- Components in this directory provide capabilities.
- `main/main.cpp` provides assembly.
- FreeRTOS tasks provide runtime scheduling.

---

## Key Dependencies

- `config/` is the global constant entry point referenced by almost every component.
- `I2C/` is shared by `sensor/` and `battery/`.
- `audio/` is primarily used by `notify/`.
- `display/` provides the low-level display capability; `ui/` builds the LVGL object tree on top of it.
- `identity/` provides MQTT username / password derivation to `net/`.
- `net/` is the external channel for `tx/`, `msg/`, `event/`, `health/`, and `profile/`.
- `settings/`, `fs/`, and `error/` are cross-cutting services shared by multiple higher-level components.

---

## Documentation Boundary

- `sensor/README_EN.md` and `sensor/README.md` are BeaconOps-authored IMU component documents.
- Most README / CHANGELOG files under `display/` come from upstream display-stack projects. They are retained with the vendored source code and are not rewritten as part of the BeaconOps document set.

---

## Suggested Reading Order

1. `config/config.h`: review pins, network settings, and credential placeholders.
2. `main/main.cpp`: understand initialization order, task model, and component wiring.
3. `identity/` + `net/`: understand device identity, HMAC authentication, and MQTT I/O.
4. `msg/` + `tx/` + `event/` + `health/` + `profile/`: understand protocol flow and reporting paths.
5. `display/` + `ui/` + `notify/` + `sensor/`: understand local interaction, prompt tones, and behavior input.