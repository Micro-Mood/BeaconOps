# BeaconOps 固件改造清单 v1

> 状态:**已实施**(代码已在 `\\wsl.localhost\Ubuntu-22.04\esp-item\BeaconOps\`,5 个 bug 已修复)
> 配套:[protocol-v1.md](./protocol-v1.md)、[data-model-v1.md](./data-model-v1.md)
> 源码根目录(WSL):`/esp-item/BeaconOps/`
> 访问路径(Win):`\\wsl.localhost\Ubuntu-22.04\esp-item\BeaconOps\`

---

## 0. 总体策略

- ✅ **删除**:`components/audio/` TTS 部分(D-03)、`hb` 相关代码、SoftAP/Captive Portal 配网。`DEVICE_UUID` 固定烧录改为运行时读 MAC。
- ✅ **新增**:`components/identity/`(MAC 读取 + config.h batch 凭证 + HMAC 计算)、`components/health/`(取代 hb)、`components/event/`(端事件上行)。
- ✅ **改写**:`components/config/config.h`、`components/net/mqtt.{h,c}`(移除 roles/role broadcast,BEFORE_CONNECT 刷新 HMAC)、`components/msg/{msg.c,parser.c}`(ack_mode 条件检查)、`components/tx/tx.c`、`main.cpp`(启动顺序 + 组件注入)。
- ✅ **保留**:`profile/`、`sensor/`、`ui/`、`display/`、`fs/`、`backlight/`、`battery/`、`I2C/`、`certs/`、`error/`、`settings/`、`decode/`、`notify/`(仅提示音,与 TTS 无关)。

---

## 1. 文件级改动清单

### 1.1 ✅ `components/config/config.h`

**删除**:
```c
#define DEVICE_UUID  "00000000-..."
#define BATCH_SECRET "REPLACE_ME_..."
#define MQTT_TOPIC_HB_FMT       "device/%s/uplink/hb"
#define MQTT_ONLINE_PAYLOAD     "{\"online\":true,\"fw\":\"BeaconOps-0.1\"}"
```

**修改**:
```c
// 新版固件版本号(LWT online payload 与 health 都用它)
#define FW_VERSION              "BeaconOps-1.0"
// MQTT broker — TLS 由 nginx stream 在 :8883 终止，后端转发本机 Mosquitto :1883
// 证书：Let's Encrypt (ISRG Root X1)，固件默认 CA 在 certs.c 已内置
#define BROKER_URI              "mqtts://YOUR_BROKER_HOST:8883"
#define MQTT_KEEPALIVE_S        60
#define MQTT_RECONNECT_MS       5000
// 移除 hb,新增 event 与 health
#define MQTT_TOPIC_CMD_FMT      "device/%s/cmd"
#define MQTT_TOPIC_ACK_FMT      "device/%s/uplink/ack"
#define MQTT_TOPIC_EVENT_FMT    "device/%s/uplink/event"
#define MQTT_TOPIC_HEALTH_FMT   "device/%s/uplink/health"
#define MQTT_TOPIC_PROFILE_FMT  "device/%s/uplink/profile"
#define MQTT_TOPIC_STATUS_FMT   "device/%s/status"

// LWT 与 online payload 改由 mqtt 组件运行时拼(含 device_id),
// 不再用宏字符串(因为含运行时 MAC)。

// HMAC 重放窗口
#define MQTT_HMAC_NONCE_BYTES   16
#define MQTT_HMAC_MAX_SKEW_S    300

// 批次凭证—编译时常量(同批设备共享;切批次 = 重烧 app)
// 生产线可生成 batch_credentials.h 覆盖这两个 #define
#define BATCH_UUID              "b2026q2-dev"
#define BATCH_SECRET            "dev-shared-secret-change-me"

// NVS 命名空间
#define NVS_NS_TX_PENDING       "tx_pending"     // tx 组件 ack 重发环
#define NVS_NS_WIFI             "wifi"           // last_good_ssid 等

// Wi-Fi 凭证—多 SSID 列表,出厂同烧,运行时扫描选信号最强者
#define WIFI_CRED_LIST          { \
    {"OfficeAP-2.4G", "..."}, \
    {"OfficeAP-5G",   "..."}, \
    {NULL, NULL} \
}
```

**保留**:管脚定义、I2C/I2S/LVGL/SPIFFS 全部不动。

### 1.2 ✅ 新建 `components/identity/`

```
components/identity/
├── CMakeLists.txt
├── identity.h          // identity_init() / identity_get_device_id() /
│                       // identity_get_batch_uuid() / identity_build_password(out, len)
└── identity.c
```

- `device_id` 12 hex:`esp_efuse_mac_get_default()` → `snprintf("%02x%02x%02x%02x%02x%02x", ...)`
- `batch_uuid` / `batch_secret`:**直接用 `config.h` 的 `BATCH_UUID` / `BATCH_SECRET` 宏**,随主固件同烧,不读 NVS。
- `identity_build_password()`:生成 nonce → 读 `time(NULL)` → `HMAC_SHA256(BATCH_SECRET, device_id||"|"||ts||"|"||nonce)` → 输出 `"ts:nonce_hex:hmac_hex"`。
- `username = BATCH_UUID`,`password = identity_build_password()`(服务器侧 split)。

### 1.3 ✅ `components/net/mqtt.{h,c}`

> bug C1/C2(删 `MQTT_TOPIC_BCAST_ROLE_FMT`、`dispatch_data` 删除 role 路由)、C3(删 `mqtt_config_t.roles` 字段)已修复。

- 删除 `publish_hb` API。
- 新增 `publish_event(payload)`、`publish_health(payload)`、`publish_status_online(payload)`。
- 改 `mqtt_config_t`:删 `topic_hb_fmt`,加 `topic_event_fmt` / `topic_health_fmt`。
- `username/password` 不再来自宏,由 main.cpp 用 `identity_hmac_password()` 在 connect 前刷新(每次重连重算 nonce/ts)。
- LWT payload 由 main 运行时构造:`{"online":false}` (retain=true);online payload `{"online":true,"fw":"BeaconOps-1.0","ts":<unix>}`。

### 1.4 ✅ `components/msg/parser.{h,c}`

**删除**:
- 嵌套 `display.title / display.body` 兼容分支
- `level` 整数兼容
- `audio_text` 字段
- 任意非 `info/notice/warn/emergency` 字符串的兼容

**保留(新版唯一支持)**:
```jsonc
{ "id":"...", "ts":..., "ttl":..., "level":"...", "title":"...", "body":"...", "ack":"..." }
```

**拒收逻辑**:
- JSON parse fail → 调用 `event_emit("parse_reject", "bad_json", "")`,丢弃
- `title=="" && body==""` → `event_emit("parse_reject", "no_display", id)`,丢弃
- `level` 不在 4 值之列 → `event_emit("parse_reject", "bad_level", id)`,丢弃
- 设置 `msg.ack_mode = ack_enum`(新增字段;默认 `none`)
- `level=warn|emergency` 时 `msg.ttl = 0`(永不过期)

`msg_t` 字段调整:
- 删 `audio_text` / `has_audio` / `display` 嵌套(若存在)
- 加 `ack_mode_e ack_mode`

### 1.5 ✅ `components/tx/tx.{h,c}`

- 改 `tx_emit_ack` 上报内容:仅 `{msg_id, kind, ts}`(删除 via/device_id/type)
- 重发达到 10 次失败时:
  - 从 NVS 删该条
  - 调用注入的 `event_publish_fn(EVENT_ACK_GIVE_UP, msg_id, kind)`
- `tx_config_t` 增加 `event_publish_fn` + `event_user` 字段
- `tx_ack_kind_e` 与下行 topic 一致:RECEIVED/DISPLAYED/ACKED/EXPIRED → 上报字段 `kind="received|displayed|acknowledged|expired"`

### 1.6 ✅ 新建 `components/event/`

```
event.h:
  typedef enum {
      EVENT_ACK_GIVE_UP,   // detail: ack kind 名
      EVENT_PARSE_REJECT,  // detail: 拒收原因
      EVENT_STORE_FULL,    // detail: SPIFFS slot kind
      EVENT_DEDUP_DROP,    // detail: ""
  } event_kind_e;

  esp_err_t event_init(event_dev_t **dev, mqtt_dev_t *mqtt, const char *device_id);
  esp_err_t event_emit(event_dev_t *dev, event_kind_e kind, const char *msg_id, const char *detail);
```

- 内部直接 publish 到 `device/{id}/uplink/event`,QoS=1
- MQTT 未连接时,**短期 RAM 队列**(8 条)+ 连上后 flush;超出丢弃并 log
- 不做 NVS 持久化(事件丢失可接受,不同于 ack)

### 1.7 ✅ 新建 `components/health/`

```
health.h:
  typedef struct {
      mqtt_dev_t *mqtt;
      battery_dev_t *battery;
      const char *device_id;
      uint32_t period_s;           // 0 → 30
      // 信号源
      int (*get_rssi)(void);
      const char *(*get_ip)(void);
      uint32_t (*get_uptime_s)(void);
      uint32_t (*get_spiffs_pending)(void);  // msg 组件
      uint32_t (*get_ack_pending)(void);     // tx 组件
      uint32_t (*get_drop_count)(void);      // parser 累计
  } health_config_t;
  esp_err_t health_init(health_dev_t **dev, const health_config_t *cfg);
```

- 后台任务每 `period_s` 秒触发,或 battery_soc 变化 ≥3% / rssi 变化 ≥10dB 立即触发(去抖 5 s)
- JSON 见 `protocol-v1.md` §3.5

### 1.8 ✅ 删除 `components/audio/` TTS 部分

- 整目录删
- 从 `main/CMakeLists.txt REQUIRES` 删 `audio`
- `notify/` 当前 include "audio.h" 来播提示音 — 这部分音频是**本地提示音**(蜂鸣序列),不是 TTS。保留 notify,但若 notify 依赖 audio.h 提供的 i2s 抽象,**保留 audio 组件但只暴露 i2s_write,删除 TTS / opus / http 流式相关代码**。
- 实际操作:进入 `components/audio/audio.c` 删除 `audio_play_url` / `audio_tts_*` / opus decoder 调用,只留 `audio_init / audio_deinit / audio_session_acquire / audio_session_release / audio_write_pcm`。

### 1.9 ✅ `main.cpp`

> bug C3(删 `qcfg.roles = nullptr`)已修复。

启动顺序(SNTP 必须在 MQTT 之前):

```cpp
1. nvs_flash_init
2. settings_init
3. i2c / sensor / battery / display / lvgl / ui_init
4. fs_init (SPIFFS)
5. msg_init / tx_init(暂不连 mqtt)
6. event_init(暂存)
7. wifi_init → 扫描 WIFI_CRED_LIST 交集,选信号最强者连
8. sntp_sync(阻塞,失败 retry,直到成功)
9. identity_init()(读 efuse MAC;batch 凭证来自 config.h)
10. mqtt_init(用 identity_build_password 拼 username/password)
11. mqtt 连上后 → tx/event/health 绑定 mqtt 句柄
12. health_init / profile_init 开跑
```

**删除**:
- `audio_tts_*` 调用
- 任何 `MQTT_TOPIC_HB_FMT` 引用
- 硬编码的 `MQTT_ONLINE_PAYLOAD` 引用
- SoftAP 配网 / Captive Portal 代码路径

### 1.10 ✅ `components/msg/msg.{h,c}`

> bug C4(`msg_step_current` 先找 `cur_slot` 再检查 ack_mode≥DISPLAYED)、C5(`msg_step_shake` 检查 ack_mode≥ACKNOWLEDGED)已修复。

- 新增 `ack_mode_e`(none/received/displayed/acknowledged)
- 删 `audio_text` / `has_audio`
- `msg.on_ack` 回调签名不变;装配层根据 `ack_mode` 决定哪些 ack 发(none 时不调 tx_emit_ack)。

### 1.11 `components/msg/lru_dedup.c`

- LRU 命中时,可选调用 `event_emit(DEDUP_DROP, msg_id, "")`(默认关,通过 Kconfig)。

### 1.12 `components/profile/profile.c`

- JSON 输出删除 `seq / battery / rssi / queue.*`(已迁到 health),仅保留行为字段。
- 字段名对齐 protocol §3.7。

### 1.13 partitions.csv

- 不需新增分区。`tx_pending` / `wifi` / `nvs.net80211` 等 NVS 命名空间均复用默认 `nvs` 分区。
- 检查 `nvs` 分区是否够大;若 4 KB 可能不够 tx_pending + last_good_ssid,改为 16 KB。

---

## 2. 批次凭证生成(可选,生产环节)

默认 `config.h` 里的 `BATCH_UUID` / `BATCH_SECRET` 是 dev 占位值。正式生产时:

1. 生产系统生成 `BATCH_UUID` + `BATCH_SECRET`(后者 ≥ 32 字节随机)。
2. 写到 `components/config/batch_credentials.h`:
   ```c
   #pragma once
   #define BATCH_UUID    "b2026q2-0001"
   #define BATCH_SECRET  "<32+ 字节 base64>"
   ```
3. 在 `config.h` 开头 `#include "batch_credentials.h"`(使宏在 `#ifndef BATCH_UUID` 之前生效;该文件加入 `.gitignore`)。
4. `idf.py build flash` 同步烧 app 即可。

同步在服务器 `POST /beacon/api/v1/batches` 创建该 batch_uuid 记录(填入同一 `batch_secret`)。

---

## 3. 实施次序

1. **批 A(无网即可验证)**:`identity` 组件、`config.h` 改写、`parser` 重写、`msg_t` 字段调整。串口看 log 验证 MAC 读取与新 JSON 解析。
2. **批 B(需服务器配合)**:`mqtt` 改 username/password 来源、`event` 组件、`health` 组件。需要服务器先实现 `/auth/device` webhook 才能完整测。
3. **批 C(收尾)**:删 `audio` TTS 代码、删 hb 引用、删旧 LWT 宏。

---

## 4. 不动的部分

- LVGL UI(显示组件)— 字段从 msg.title/body 直接映射
- sensor_task(摇晃检测)— ack 触发链路不变
- behavior / EC-BAID — 输出表名改 `device_id`(在服务器侧,固件不改)
- backlight、battery、I2C、display、settings、fs — 全部不动

---

**文档结束**
