# Source Guide

`src/` is the top-level source directory of the open-source BeaconOps project. It is roughly split into three parts:

- `Hardware/`: the hardware side, including the PCB project, enclosure models, firmware source, and flashing scripts
- `Frontend/`: the web console
- `Backend/`: the management API, MQTT bridge, and persistence layer

This document ties those three parts together and then writes down a set of deployment conventions that are **independent of any specific server layout**. In other words, it describes what interfaces and paths the project needs, not what my own server happens to look like.

---

## 1. Directory Layout

```text
src/
├── Hardware/
│   ├── Firmware/          ESP-IDF firmware project
│   └── Scripts/           Windows flashing scripts
├── Frontend/              Vue 3 + Vite console
└── Backend/               FastAPI + SQLite + gmqtt server
```

Recommended reading order:

1. Read this document first to understand how the three parts fit together.
2. Then read `Backend/README_EN.md` to confirm the API, MQTT, and environment variables.
3. Then read `Frontend/README_EN.md` to confirm console build and mount paths.
4. Then read `Hardware/README_EN.md` to understand the hardware-side layout.
5. Then read `Hardware/Firmware/README_EN.md` to confirm firmware build, credentials, and runtime behavior.
6. Finally read `Hardware/Firmware/components/README_EN.md` and `Hardware/Firmware/components/sensor/README_EN.md` for the firmware component layer.

---

## 2. System Components

BeaconOps consists of three ends:

### 2.1 Hardware

The hardware side is based on ESP32-C3 and runs ESP-IDF v5.x firmware. It is responsible for:

- preserving the PCB design source file
- preserving the enclosure STL / 3DM models
- connecting to Wi-Fi and the MQTT broker
- receiving downlink commands and presenting them through the local UI and audio path
- reporting online status, health, events, and profile data
- generating the HMAC authentication password from batch credentials

Detailed hardware documentation:

- `Hardware/README_EN.md`
- `Hardware/PCB/README_EN.md`
- `Hardware/Enclosure/README_EN.md`
- `Hardware/Firmware/README_EN.md`
- `Hardware/Scripts/README_EN.md`
- `Hardware/Firmware/components/README_EN.md`
- `Hardware/Firmware/components/sensor/README_EN.md`

### 2.2 Backend

The backend is built with FastAPI + SQLite + gmqtt. It is responsible for:

- admin login and Session / CSRF handling
- management of batches, devices, messages, admins, and audit logs
- receiving device uplinks and sending downlinks as the MQTT bridge
- validating device MQTT access authentication
- providing REST APIs and event streams for the frontend console

Detailed backend documentation:

- `Backend/README_EN.md`

### 2.3 Frontend

The frontend is built with Vue 3 + Vite + TypeScript. It is responsible for:

- admin login
- device, batch, message, and audit views
- realtime event display
- console theme and interaction layer

Detailed frontend documentation:

- `Frontend/README_EN.md`

---

## 3. Runtime Relationship

The system data path can be summarized as:

```text
Hardware (ESP32-C3)
    │
    │ MQTT over TLS
    ▼
MQTT Broker
    │
    │ bridge subscribe / publish
    ▼
Backend (FastAPI)
    │
    │ REST + event stream
    ▼
Frontend (Vue console)
```

More concretely:

1. The firmware connects to the MQTT broker through `BROKER_URI`.
2. The device uses `BATCH_UUID` as the MQTT username, and generates its password locally using the HMAC rules.
3. The backend connects to the broker with a bridge account, subscribes to device uplink topics, stores data in the database, and serves query interfaces to the frontend.
4. The frontend talks to the backend through `/beacon/api/v1` and receives realtime updates through the event stream.
5. Downlink messages are created from the frontend, sent to the backend, and then published by the backend to device topics through the broker.

---

## 4. Responsibility Boundaries

### 4.1 Boundary Between Hardware and Backend

Hardware does not store admin accounts and does not expose an HTTP service directly.

The only local configuration required on the hardware side is:

- `BATCH_UUID`
- `BATCH_SECRET`
- `WIFI_CRED_LIST`
- `BROKER_URI`

The batch secret is only used for device authentication. Batch creation, revocation, rotation, device assignment, and message history are all maintained by the backend.

### 4.2 Boundary Between Frontend and Backend

The frontend does not embed real accounts, passwords, or tokens.

The frontend depends on the backend for:

- login APIs
- batch, device, message, and audit APIs
- Cookie-based sessions
- CSRF cookie / header
- event streaming

The frontend itself is responsible only for UI and interaction. It is not the final trusted store of authentication state.

### 4.3 Deployment Boundary

The open-source directory does not include deployment directories, certificates, databases, or logs tied to a personal server layout. The project only relies on the following external contracts:

- Frontend public path: `/beacon/console/`
- Backend API prefix: `/beacon/api/v1`
- Backend health check: `/beacon/health`
- Device broker entry: `mqtts://YOUR_BROKER_HOST:8883`

Whether deployment uses nginx, Caddy, Traefik, broker-native TLS, Docker, systemd, or another orchestration method is up to the deployer.

---

## 5. Local Development

### 5.1 Backend

Inside `Backend/`:

```bash
pip install -r requirements.txt
copy .env.example .env
python -m uvicorn app.main:app --host 127.0.0.1 --port 8020
```

You need to fill the following values in `.env`:

- `ADMIN_USERNAME`
- `ADMIN_PASSWORD_HASH`
- `JWT_SECRET`
- `MQTT_HOST`
- `MQTT_PORT`
- `MQTT_USERNAME`
- `MQTT_PASSWORD`
- `AUTH_WEBHOOK_TOKEN` (optional)

### 5.2 Frontend

Inside `Frontend/`:

```bash
npm install
npm run dev
```

In development, the frontend proxies `/beacon/api/*` to the local backend:

```text
http://127.0.0.1:8020
```

If you need a temporary local login bypass only on your development machine, use:

```text
.env.development.local
```

and set:

```env
VITE_BYPASS_LOGIN=true
```

### 5.3 Hardware

The hardware build flow is hybrid:

- compile the ESP-IDF firmware inside WSL
- run the flashing scripts on Windows

Before entering `Hardware/Firmware/`, fill in the placeholders in `components/config/config.h`, then follow the build and flashing steps described in `Hardware/README.md`.

---

## 6. Build Outputs

### 6.1 Frontend Build

Inside `Frontend/`:

```bash
npm run build
```

The default output is a static site intended to be served under `/beacon/console/`.

### 6.2 Hardware Build

Inside WSL, enter `Hardware/Firmware/` and run:

```bash
bash build.sh <your-Windows-username>
```

The script builds the firmware and copies the merged `main.bin` to the Windows desktop directory, where it can be flashed with `Hardware/Scripts/flash.bat`.

### 6.3 Backend Runtime

The backend is not a frontend-style build artifact. It runs directly as a Python service.

If you want the backend to serve the built frontend bundle too, place the frontend build output under:

```text
Backend/console/
```

`Backend/app/main.py` will automatically expose `/beacon/console/` when that directory exists.

---

## 7. General Deployment Plan

This section describes project-level deployment conventions, not the directory layout of a specific server.

### 7.1 Minimum Runnable Topology

```text
Internet
   │
   ├── HTTPS -> Reverse Proxy -> Frontend static (/beacon/console/)
   │                          -> Backend API    (/beacon/api/v1)
   │                          -> Health         (/beacon/health)
   │
   └── MQTTS -> Broker (:8883)
                │
                └── Backend bridge connection
```

### 7.2 Production Requirements

1. Expose a broker address reachable by devices, such as `YOUR_BROKER_HOST:8883`.
2. Configure TLS for that entry point.
3. Start the backend and provide valid bridge credentials and a JWT secret.
4. Build the frontend and publish it under `/beacon/console/`.
5. Route `/beacon/api/v1` to the backend.
6. Route `/beacon/health` to the backend health endpoint.
7. Change `BROKER_URI` in the firmware to the production broker address and rebuild / reflash the device.

### 7.3 Recommended Deployment Order

1. Deploy the MQTT broker first and verify that the backend can connect to it.
2. Start the backend and confirm that `/beacon/health` is working.
3. Build and publish the frontend.
4. Finally write firmware credentials and flash devices.

### 7.4 Route Contracts

No matter which web server is used, it is recommended to keep these paths unchanged:

- `/beacon/console/`: frontend console
- `/beacon/api/v1/*`: backend API
- `/beacon/health`: health check

This keeps the frontend, backend, and the default documentation conventions aligned and reduces the amount of rework needed for deployment.

---

## 8. Hardware Deployment Notes

The hardware side is not simply “flash and run”. The following prerequisites are required:

1. valid batch credentials are prepared
2. a usable Wi-Fi list is prepared
3. a broker TLS endpoint is prepared
4. if a custom CA file is enabled, certificate materials are provided locally

The defaults in the open-source firmware directory are placeholders only. They do not contain real Wi-Fi credentials, batch secrets, or production broker addresses.

---

## 9. Security and Sensitive Data

The following items have been removed from the open-source directory:

- real `.env` / `.env.local` files
- backend databases, logs, and runtime cache files
- real batch secrets, Wi-Fi credentials, and broker addresses from the firmware
- frontend local development environment files
- certificate files and machine-specific operations directories

For local deployment, you still need to provide your own:

- admin password hash
- JWT secret
- MQTT bridge username and password
- device batch credentials
- Wi-Fi credentials
- broker TLS configuration

---

## 10. Further Reading

The following files are the project-authored BeaconOps documentation set. README / CHANGELOG files under `Hardware/Firmware/components/display/` are upstream third-party documents retained with vendored source code and are not reorganized as part of the BeaconOps docs.

- `Hardware/README_EN.md`
- `Hardware/PCB/README_EN.md`
- `Hardware/Enclosure/README_EN.md`
- `Hardware/Firmware/README_EN.md`
- `Hardware/Scripts/README_EN.md`
- `Hardware/Firmware/components/README_EN.md`
- `Hardware/Firmware/components/sensor/README_EN.md`
- `Frontend/README_EN.md`
- `Backend/README_EN.md`

If you just want to get the system running quickly, start with:

1. `Backend/README_EN.md`
2. `Frontend/README_EN.md`
3. `Hardware/README_EN.md`

If you are preparing a real device deployment, continue with:

1. `Hardware/Firmware/README_EN.md`
2. `Hardware/Scripts/README_EN.md`