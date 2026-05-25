# Hardware

> Windows 原生编译 ESP-IDF 速度极慢，我把编译丢到 WSL 里做。但 WSL 又看不见 Windows 上的 USB，烧录还得回 Windows。所以这套流程不是“设计出来的优雅方案”，纯粹是在两边的限制里找了个能跑的折中。

BeaconOps 硬件端当前由四块组成：PCB 工程（`PCB/`）、外壳模型（`Enclosure/`）、固件源码（`Firmware/`）和烧录脚本（`Scripts/`）。

PCB 是去年就画好的。主芯片 ESP32-C3 是 QFN-32，外置 flash Winbond W25Q128JV 是 WSON-8；IMU LSM6DS3TR-C 是 LGA-14（底部焊盘，偏 0.1 mm 虚焊）；音频功放 MAX98357AEWL+ 是 WLP-9（BGA 类，返修靠 X-Ray）。被动器件清一色 0402，最窄间距约 0.4 mm。复刻这块板靠的不是文件，是手艺。外壳直接复用了 [pocket](../../../pocket/hardware) 的那一套，因为尺寸一样，没必要重做。

## 文档导航

- 英文总览：`README_EN.md`
- PCB 说明：`PCB/README.md`
- 外壳说明：`Enclosure/README.md`
- 固件总览：`Firmware/README.md`
- 固件英文版：`Firmware/README_EN.md`
- components 总览：`Firmware/components/README.md`
- sensor 组件说明：`Firmware/components/sensor/README.md`
- 烧录脚本说明：`Scripts/README.md`
- 烧录脚本英文版：`Scripts/README_EN.md`

---

## 环境要求

- **Windows** — 执行烧录脚本（`Scripts\`）
- **WSL**（Ubuntu 22.04 推荐）— 编译固件，需安装 ESP-IDF 工具链
- **ESP-IDF v5.x** — 安装在 WSL 内
- **设备**：ESP32-C3，通过 USB 连接到 Windows 主机

编译在 WSL 内完成，产物通过 `/mnt/c/` 传递到 Windows 侧后烧录。

---

## 目录结构

```
Hardware/
├── PCB/                PCB 工程（嘉立创专业版）
│   ├── ProPrj_BeaconOps.epro
│   └── README.md
├── Enclosure/          外壳模型
│   ├── shell.stl
│   ├── back-cover.stl
│   ├── model.3dm
│   ├── model_embedded_files/
│   └── README.md
├── Firmware/           固件源码（ESP-IDF 项目）
│   ├── components/     自定义组件
│   ├── main/           主程序入口
│   ├── CMakeLists.txt
│   ├── build.sh        编译脚本（在 WSL 内运行）
│   ├── partitions.csv  分区表
│   ├── sdkconfig       ESP-IDF 编译配置
│   └── dependencies.lock
└── Scripts/            烧录脚本（在 Windows 下运行）
    ├── flash.bat       烧录固件
    ├── fs.bat          打包并烧录 SPIFFS 文件系统
    ├── spiffs.py       SPIFFS 镜像生成工具
    ├── esptool.exe     esptool Windows 可执行文件
    └── README.md
```

---

## PCB 与外壳

- `PCB/ProPrj_BeaconOps.epro` 是 BeaconOps 的嘉立创专业版 PCB 工程源文件。
- `Enclosure/` 中保留了当前硬件使用的外壳模型。
- 当前外壳采用与 `pocket/hardware` 相同的壳体方案，保留了可直接打印的 `shell.stl`、`back-cover.stl`，以及可编辑源文件 `model.3dm`。
- 如需修改结构件，优先在 `model.3dm` 上改，再重新导出 STL。

---

## 首次环境搭建（WSL）

```bash
# 安装 ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh esp32c3
. ./export.sh
```

每次打开新终端编译前，需激活工具链：

```bash
. ~/esp/esp-idf/export.sh
```

---

## 构建流程

### 1. 填写凭证

编译前先在 `Firmware/components/config/config.h` 中填写实际值（见下方[凭证配置](#凭证配置)）。

### 2. 编译（WSL）

将 `Firmware/` 目录放到 WSL 内任意位置，进入后执行 `build.sh`，参数为当前 Windows 用户名：

```bash
bash build.sh <你的Windows用户名>
```

脚本会：

1. 调用 `idf.py build` 编译固件
2. 调用 `esptool.py merge_bin` 将 bootloader、app、分区表合并为 `merged-binary.bin`
3. 将产物复制到 `C:\Users\<用户名>\Desktop\BeaconOps\main.bin`

目标目录 `Desktop\BeaconOps\` 需提前创建，将 `Scripts\` 放在其中。

### 3. 烧录固件（Windows）

确保设备已通过 USB 连接，进入 `Scripts\` 目录执行：

```bat
flash.bat [CHIP] [FREQ] [PORT]
```

| 参数 | 默认值 | 说明 |
|---|---|---|
| CHIP | `esp32c3` | 芯片型号 |
| FREQ | `80m` | Flash 频率 |
| PORT | `COM3` | 串口号（设备管理器中查看） |

`flash.bat` 从 `Scripts\` 的上层目录读取 `main.bin`，即 `build.sh` 的输出位置。

```bat
:: 使用默认参数
flash.bat

:: 指定串口
flash.bat esp32c3 80m COM5
```

### 4. 烧录文件系统（可选）

如需更新 SPIFFS 分区内容，在 `Scripts\` 目录下执行：

```bat
fs.bat <内容目录> [CHIP] [FREQ] [OFFSET] [SIZE] [PORT]
```

`fs.bat` 先用 `spiffs.py` 将指定目录打包为镜像，再烧录到 SPIFFS 分区（默认 offset `0x190000`，size `0x270000`）。

---

## 凭证配置

`Firmware/components/config/config.h` 中以下字段需在本地填写：

```c
#define BATCH_UUID    "YOUR_BATCH_UUID"    // 批次标识符
#define BATCH_SECRET  "YOUR_BATCH_SECRET"  // 批次 HMAC 密钥

#define WIFI_CRED_LIST { \
    {"YOUR_SSID", "YOUR_PASSWORD"}, \
    {NULL, NULL} \
}
#define BROKER_URI    "mqtts://YOUR_BROKER_HOST:8883"  // MQTT Broker 地址
```

**不要将真实凭证提交到版本库。**

---

## 进一步阅读

- `Firmware/README.md`
- `PCB/README.md`
- `Enclosure/README.md`
- `Firmware/components/README.md`
- `Firmware/components/sensor/README.md`
- `Scripts/README.md`
