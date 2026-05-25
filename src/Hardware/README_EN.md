# Hardware

> ESP-IDF builds on native Windows are painfully slow, so I compile inside WSL. But WSL cannot see Windows USB devices, so flashing has to go back to Windows. This split is not an elegant design — it is just what actually works given both constraints.

The hardware side of BeaconOps currently has four parts: the PCB project (`PCB/`), enclosure models (`Enclosure/`), firmware source (`Firmware/`), and flashing scripts (`Scripts/`).

The PCB was already drawn last year. The main chip ESP32-C3 is QFN-32; the external flash Winbond W25Q128JV is WSON-8. The IMU LSM6DS3TR-C is LGA-14 (bottom pads, a 0.1 mm shift causes a cold joint); the audio amp MAX98357AEWL+ is WLP-9 (BGA-class, rework requires X-Ray). Passive components are all 0402, tightest spacing around 0.4 mm. Reproducing this board is less about the files and more about your soldering skills. The enclosure is reused directly from [pocket](../../../pocket/hardware) — same dimensions, no reason to redesign.

## Documentation Map

- Chinese overview: `README.md`
- PCB guide: `PCB/README_EN.md`
- Enclosure guide: `Enclosure/README_EN.md`
- Firmware overview: `Firmware/README_EN.md`
- Firmware Chinese version: `Firmware/README.md`
- Components overview: `Firmware/components/README_EN.md`
- Sensor component: `Firmware/components/sensor/README_EN.md`
- Flashing scripts: `Scripts/README_EN.md`
- Flashing scripts Chinese version: `Scripts/README.md`

---

## Requirements

- **Windows** — runs the flashing scripts (`Scripts\`)
- **WSL** (Ubuntu 22.04 recommended) — compiles the firmware; requires ESP-IDF toolchain
- **ESP-IDF v5.x** — installed inside WSL
- **Target device**: ESP32-C3, connected to the Windows host via USB

Compilation happens inside WSL; the build artifact is passed to the Windows side via `/mnt/c/` and then flashed.

---

## Directory Structure

```
Hardware/
├── PCB/                PCB project (JLCEDA Pro)
│   ├── ProPrj_BeaconOps.epro
│   └── README_EN.md
├── Enclosure/          enclosure models
│   ├── shell.stl
│   ├── back-cover.stl
│   ├── model.3dm
│   ├── model_embedded_files/
│   └── README_EN.md
├── Firmware/           Firmware source (ESP-IDF project)
│   ├── components/     Custom components
│   ├── main/           Application entry point
│   ├── CMakeLists.txt
│   ├── build.sh        Build script (run inside WSL)
│   ├── partitions.csv  Partition table
│   ├── sdkconfig       ESP-IDF build configuration
│   └── dependencies.lock
└── Scripts/            Flashing scripts (run on Windows)
    ├── flash.bat       Flash firmware
    ├── fs.bat          Pack and flash SPIFFS filesystem
    ├── spiffs.py       SPIFFS image generator
    ├── esptool.exe     esptool Windows executable
    └── README.md
```

---

## PCB and Enclosure

- `PCB/ProPrj_BeaconOps.epro` is the BeaconOps PCB source project for JLCEDA Pro.
- `Enclosure/` stores the enclosure assets used by the current hardware build.
- The enclosure reuses the same shell design as `pocket/hardware`, including printable `shell.stl`, `back-cover.stl`, and the editable `model.3dm` source file.
- If you need to adjust the mechanical structure, edit `model.3dm` first and then re-export the STL files.

---

## First-Time Setup (WSL)

```bash
# Install ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh esp32c3
. ./export.sh
```

Activate the toolchain each time you open a new terminal:

```bash
. ~/esp/esp-idf/export.sh
```

---

## Build & Flash Workflow

### 1. Fill in credentials

Before building, edit `Firmware/components/config/config.h` with your actual values (see [Credentials](#credentials) below).

### 2. Compile (WSL)

Place `Firmware/` anywhere inside WSL, then run `build.sh` with your Windows username as the argument:

```bash
bash build.sh <your-windows-username>
```

The script will:

1. Run `idf.py build` to compile the firmware
2. Run `esptool.py merge_bin` to merge bootloader, app, and partition table into a single `merged-binary.bin`
3. Copy the merged binary to `C:\Users\<username>\Desktop\BeaconOps\main.bin`

Create the `Desktop\BeaconOps\` directory in advance and place `Scripts\` inside it.

### 3. Flash firmware (Windows)

Connect the device via USB, then run from the `Scripts\` directory:

```bat
flash.bat [CHIP] [FREQ] [PORT]
```

| Argument | Default | Description |
|---|---|---|
| CHIP | `esp32c3` | Chip model |
| FREQ | `80m` | Flash frequency |
| PORT | `COM3` | COM port (check Device Manager) |

`flash.bat` reads `main.bin` from the directory above `Scripts\`, which is where `build.sh` places the output.

```bat
:: Use defaults
flash.bat

:: Specify port
flash.bat esp32c3 80m COM5
```

### 4. Flash filesystem (optional)

To update the SPIFFS partition contents, run from `Scripts\`:

```bat
fs.bat <content-dir> [CHIP] [FREQ] [OFFSET] [SIZE] [PORT]
```

`fs.bat` packs the specified directory into an image using `spiffs.py`, then flashes it to the SPIFFS partition (default offset `0x190000`, size `0x270000`).

---

## Credentials

Fill in the following fields in `Firmware/components/config/config.h` before building:

```c
#define BATCH_UUID    "YOUR_BATCH_UUID"    // Batch identifier
#define BATCH_SECRET  "YOUR_BATCH_SECRET"  // Batch HMAC key

#define WIFI_CRED_LIST { \
    {"YOUR_SSID", "YOUR_PASSWORD"}, \
    {NULL, NULL} \
}

#define BROKER_URI    "mqtts://YOUR_BROKER_HOST:8883"  // MQTT broker address
```

**Do not commit real credentials to version control.**

---

## Further Reading

- `Firmware/README_EN.md`
- `PCB/README_EN.md`
- `Enclosure/README_EN.md`
- `Firmware/components/README_EN.md`
- `Firmware/components/sensor/README_EN.md`
- `Scripts/README_EN.md`
