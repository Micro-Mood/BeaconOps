# ESP 烧录工具 (Scripts)

这是一套通用的 ESP-IDF 烧录脚本，不只为 BeaconOps 服务。把整个 `Scripts/` 拷贴到项目根目录就能用，里面不需要改什么东西。主要是为了避免每次调试都手敲一堆 esptool 参数。

## 目录结构

```
<项目根>/
├── main.bin        ← 固件合并包（WSL build.sh 生成后复制到此）
└── Scripts/
    ├── flash.bat   ← 固件烧录
    ├── fs.bat      ← SPIFFS 打包 + 烧录
    ├── spiffs.py   ← SPIFFS 镜像生成工具（ESP-IDF spiffsgen）
    ├── fs.bin      ← SPIFFS 打包产物（fs.bat 自动生成）
    └── README.md   ← 本文档
```

SPIFFS 内容目录可为任意路径，烧录时作为参数传入。

---

## flash.bat — 烧录固件

将上层目录的 `main.bin` 通过 esptool 一键烧录到设备。

**用法**:
```bat
flash.bat [CHIP] [FREQ] [PORT]
```

| 参数 | 说明 | 默认值 |
|---|---|---|
| CHIP | 芯片型号 | `esp32c3` |
| FREQ | Flash 频率 | `80m` |
| PORT | 串口号 | `COM3` |

`main.bin` 不存在时自动退出并提示路径。

**示例**:
```bat
:: 默认参数
flash.bat

:: 指定芯片和串口
flash.bat esp32s3 80m COM5
```

---

## fs.bat — SPIFFS 打包 + 烧录

**两步操作**：
1. 调用 `spiffs.py` 将指定目录打包为 `fs.bin`
2. 直接调用 esptool 将 `fs.bin` 烧录到 SPIFFS 分区

**用法**:
```bat
fs.bat <SPIFFS_DIR> [CHIP] [FREQ] [OFFSET] [SIZE] [PORT]
```

| 参数 | 说明 | 默认值 |
|---|---|---|
| SPIFFS_DIR | SPIFFS 内容目录（**必填**） | 无 |
| CHIP | 芯片型号 | `esp32c3` |
| FREQ | Flash 频率 | `80m` |
| OFFSET | SPIFFS 分区起始偏移 | `0x190000` |
| SIZE | 分区大小 | `0x270000` |
| PORT | 串口号 | `COM3` |

> OFFSET 和 SIZE 需与项目 `partitions.csv` 中 storage 分区一致。

**示例**:
```bat
:: 指定 SPIFFS 目录，其余使用默认参数
fs.bat ..\FS\SPIFFS

:: 完整参数
fs.bat ..\FS\SPIFFS esp32c3 80m 0x190000 0x270000 COM5
```

目录不存在或打包失败时自动停止，不会继续执行烧录。

---

## 注意事项

- `esptool.exe` 已内置于 `Scripts/` 目录，脚本自动引用，无需加入系统 PATH
- `python` 命令需可用，无额外依赖（spiffs.py 仅用标准库）
- 烧录前确认设备已进入下载模式（按住 BOOT 上电，或芯片支持自动复位）
- `main.bin` 由 WSL 端 `build.sh <Windows用户名>` 生成并自动复制到桌面项目目录
- 建议顺序：先烧录 `fs.bat`（文件系统）再烧录 `flash.bat`（固件）