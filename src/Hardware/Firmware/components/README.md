# Components

BeaconOps 的业务逻辑几乎全部装在 `components/` 里。`main/main.cpp` 只负责把硬件初始化、推送句柄、挂回调这些事情串起来——真正能调、能改、能复用的部分都在这里。如果你想看“这东西在干什么”，从这里入手比看 main 快。

## 文档导航

- 上层固件总览：`../README.md`
- 英文版：`README_EN.md`
- sensor 组件说明：`sensor/README.md`
- sensor 英文版：`sensor/README_EN.md`

---

## 分层结构

```
components/
├── config/         全局配置：引脚、MQTT、批次凭证、Wi-Fi、SNTP
├── I2C/            I2C 总线管理器，供多设备共享
│
├── 硬件驱动
│   ├── sensor/     LSM6DS3TR-C IMU 驱动
│   ├── battery/    CW2017 电量计驱动
│   ├── audio/      MAX98357 I2S 音频输出
│   ├── backlight/  背光 PWM 控制
│   └── display/    ST7789 + LVGL 9.3 显示栈
│
├── 本地服务
│   ├── fs/         NVS / SPIFFS 封装
│   ├── settings/   持久化设置
│   ├── error/      错误记录与日志持久化
│   ├── decode/     PNG / JPG 图片解码
│   └── certs/      TLS 根证书嵌入
│
├── 协议与联网
│   ├── identity/   device_id 与 HMAC 鉴权密码生成
│   ├── net/        MQTT 客户端与 topic 收发
│   ├── msg/        下行消息解析与去重
│   ├── tx/         ACK 持久化与补发
│   ├── event/      异常/事件上报
│   └── health/     周期健康上报
│
└── 业务输出
    ├── profile/    行为聚合与 profile_delta 发布
    ├── notify/     提示音合成与播放调度
    └── ui/         屏幕 UI、消息卡片、状态显示
```

---

## 装配关系

系统启动时的职责边界如下：

1. `main/main.cpp` 同步初始化 GPIO、I2C、IMU、显示、背光、NVS、SPIFFS、音频、电量计等硬件与基础服务。
2. `main/main.cpp` 创建 `error`、`notify`、`ui`、`wifi` 句柄，并把各组件通过回调方式串起来。
3. `net_bringup_task` 在后台顺序完成 `wifi -> sntp -> identity/hmac -> mqtt -> tx/msg/profile` 的联网拉起。
4. `sensor_task` 独立运行，把 IMU 数据转换为摇晃、运动、行为事件，再回调到 `msg` 与 `profile`。

因此：

- 本目录中的组件负责“能力”。
- `main/main.cpp` 负责“装配”。
- FreeRTOS 任务负责“运行时调度”。

---

## 关键依赖

- `config/` 是全局常量入口，几乎所有组件都会引用。
- `I2C/` 被 `sensor/` 和 `battery/` 复用。
- `audio/` 主要被 `notify/` 调用。
- `display/` 提供底层显示能力，`ui/` 在其上构建 LVGL 对象树。
- `identity/` 为 `net/` 提供 MQTT 用户名/密码派生能力。
- `net/` 是 `tx/`、`msg/`、`event/`、`health/`、`profile/` 的对外通道。
- `settings/`、`fs/`、`error/` 是横切服务，会被多个业务组件共享。

---

## 文档边界

- `sensor/README.md` 与 `sensor/README_EN.md` 记录的是 BeaconOps 自己维护的 IMU 组件说明。
- `display/` 目录下的 README / CHANGELOG 大多来自上游显示栈项目，随源码保留，不在本项目文档集中重写。

---

## 建议阅读顺序

1. `config/config.h`：先看硬件引脚、网络参数和凭证占位符。
2. `main/main.cpp`：理解初始化顺序、任务模型和组件装配关系。
3. `identity/` + `net/`：理解设备身份、HMAC 鉴权和 MQTT 收发。
4. `msg/` + `tx/` + `event/` + `health/` + `profile/`：理解协议栈与数据上报路径。
5. `display/` + `ui/` + `notify/` + `sensor/`：理解本地交互、提示音与行为输入。


