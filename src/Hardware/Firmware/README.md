# Firmware

BeaconOps 的固件跑在 **ESP32-C3** 上，基于 **ESP-IDF v5.x**，只有 **4 MB Flash**，所以顶层应用被压到 2.87 MB以内，资源扣得比较紧。UI 那块用的是 **LVGL 9.3**，跑在 FreeRTOS 上。

> 不要随手换更大的 Flash 型号，分区表、字体、SPIFFS 布局都是按 4 MB 调过的，换了得重新算账。

## 文档导航

- 上层硬件总览：`../README.md`
- 英文版：`README_EN.md`
- components 总览：`components/README.md`
- components 英文版：`components/README_EN.md`
- sensor 组件说明：`components/sensor/README.md`
- sensor 英文版：`components/sensor/README_EN.md`

---

## 分区表

```
Name        Type    SubType   Offset      Size
nvs         data    nvs       0x9000      24 KB    NVS 键值存储
phy_init    data    phy       0xF000       4 KB    RF 校准数据
factory     app     factory   0x10000   2.87 MB    应用固件
storage     data    spiffs    (自动)       1 MB    SPIFFS 文件系统
```

当前应用二进制约占 **2.4 MB**，`factory` 分区分配了 2.87 MB，余量约 470 KB。  
增加功能或组件时请留意分区空间，必要时修改 `partitions.csv`。

---

## 组件一览

```
components/
├── config/         系统配置（引脚、MQTT、批次凭证）
├── I2C/            I2C 总线管理器（多设备共享总线）
│
├── 硬件驱动
│   ├── audio/      MAX98357 I2S 音频驱动
│   ├── backlight/  PWM 背光控制（空闲关屏、运动唤醒、紧急强亮）
│   ├── battery/    CW2017 电量计驱动（I2C）
│   ├── sensor/     LSM6DS3TR-C IMU 驱动（加速度 + 陀螺仪）
│   └── display/    ST7789 显示驱动 + LVGL 9.3
│
├── 系统服务
│   ├── certs/      内嵌 TLS CA 证书（需自行提供，见下方说明）
│   ├── decode/     PNG / JPG 图片解码（绑定 LVGL canvas）
│   ├── error/      统一错误诊断（环形缓冲 + SPIFFS 日志持久化）
│   ├── fs/         NVS 工具库 + SPIFFS 挂载管理
│   └── settings/   应用配置持久化（设备名、Wi-Fi SSID 等）
│
├── 协议栈
│   ├── identity/   设备身份 + HMAC 鉴权（MAC → device_id，批次密钥生成 MQTT 密码：`<ts>:<nonce>:<HMAC_SHA256>`）
│   ├── net/        MQTT 客户端（TLS，protocol v1 通道定义；上线时发 LWT，在线时立即发 status=online）
│   ├── msg/        消息接收解析 + LRU 去重缓存；warn / emergency 在解析阶段强制置为「需确认」
│   ├── tx/         出站 ACK 服务（NVS 环形缓冲 + 指数退避重试；超限后升级为 ack_give_up event）
│   ├── event/      设备事件上报（parse_reject / store_full / ack_give_up 等异常）
│   └── health/     周期健康上报（默认 30 s；电量或充电状态变化时额外触发一次）
│
├── 业务层
│   ├── profile/    60 秒滚动行为聚合器（发布 profile_delta；离线期间写 SPIFFS，上线后补发）
│   ├── notify/     提示音组件（info / notice / warn / emergency 四级 + 确认音）
│   └── ui/         屏幕 + 消息卡片栈（LVGL；默认字体为自定义 MiSans Bold GB2312 位图字库，由 generate_misans_font.py 生成，直接编进固件）
│
└── README.md
```

---

## 文档边界

- `components/README.md` 与 `components/sensor/README.md` 是 BeaconOps 自己维护的固件项目文档。
- `components/display/lv_v9.3/` 与 `components/display/lv_port/` 下的 README / CHANGELOG 属于第三方上游文档，随源码保留。

---

## 启动流程与任务关系

`main/main.cpp` 是装配入口，不承载具体业务逻辑。启动时大致分为四个阶段：

1. **同步初始化硬件与本地服务**  
    依次初始化 GPIO、I2C、IMU、LVGL、背光、NVS、settings、SPIFFS、audio、CW2017、电源管理等基础设施。

2. **同步创建本地业务句柄**  
    创建 `error`、`notify`、`ui` 与 `wifi` 句柄。这里的 `wifi` 只完成句柄创建，不在主线程里直接联网。

3. **后台任务 `net_bringup_task` 拉起联网链路**  
    按顺序执行 `wifi -> sntp -> identity/hmac -> mqtt -> tx/msg/profile`。即使联网失败，本地 UI 仍然可以独立运行。

4. **独立任务 `sensor_task` 处理行为输入**  
    持续读取 IMU 数据，把摇晃、运动、行为事件通过回调转发给 `msg` 和 `profile`。

可以把运行时结构理解为：

- `main/main.cpp` 负责初始化与装配
- `components/` 负责功能实现
- FreeRTOS 任务负责联网与传感器采样调度

---

## 消息处理与确认链路

下行消息到达后由 `msg/` 解析：

- `info` / `notice`：解析后直接标记为「已收到」，不需要用户操作
- `warn` / `emergency`：解析阶段强制标记为「需要确认」，必须用户摇一摇才算完成

摇一摇的回调链：`sensor_task` 检测到手势 → `msg_on_shake` → `tx/` 发布 ACK

ACK 写入 NVS 环形缓冲，带指数退避重试。重试超限后 `tx/` 不静默丢弃，而是通过 `event/` 上报一条 `ack_give_up` 事件，服务端可以观察到这台设备的确认失败记录。

---

## 设备身份与鉴权

每台设备的 `device_id` 在运行时从 eFuse 读取 MAC 地址生成（12 位 hex），无需烧录。

MQTT 连接鉴权使用批次凭证（`BATCH_UUID` / `BATCH_SECRET`），同批次设备共享，编译时内嵌。鉴权密码格式为：

```
<timestamp>:<nonce>:<HMAC-SHA256(batch_secret, device_id|ts|nonce)>
```

详见 `components/identity/identity.h`。

---

## TLS 证书（需自行提供）

`components/certs/` 目录下需放置 Broker CA 根证书，文件名固定为 `isrg_root_x1.pem`，由编译系统通过 `EMBED_TXTFILES` 内嵌到固件。

若 Broker 使用 **Let's Encrypt** 签发的证书，下载 ISRG Root X1：

```
https://letsencrypt.org/certificates/
```

下载 "ISRG Root X1 (PEM)" 并保存为 `components/certs/isrg_root_x1.pem`。

使用其他 CA 时，替换为对应的根证书即可，无需改动代码。

---

## 凭证配置

编译前在 `components/config/config.h` 中填写：

```c
#define BATCH_UUID    "YOUR_BATCH_UUID"
#define BATCH_SECRET  "YOUR_BATCH_SECRET"

#define WIFI_CRED_LIST { \
    {"YOUR_SSID", "YOUR_PASSWORD"}, \
    {NULL, NULL} \
}
```

同时确认 MQTT Broker 地址：

```c
#define BROKER_URI   "mqtts://YOUR_BROKER_HOST:8883"
```

**不要将真实凭证提交到版本库。**
