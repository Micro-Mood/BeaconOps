# Source Guide

`src/` 是 BeaconOps 的开源源码总目录，主要分三块：

- `Hardware/`：硬件端，包括 PCB 工程、外壳模型、固件源码和烧录脚本
- `Frontend/`：Web 控制台
- `Backend/`：管理 API、MQTT bridge 和持久层

这份文档把这三块串起来讲一遍，再顺手写一套**与具体服务器架构无关**的部署约定。也就是说，这里说的是“这个项目需要满足哪些接口和路径”，不是“我那台服务器的目录长什么样”。

---

## 1. 目录结构

```text
src/
├── Hardware/
│   ├── Firmware/          ESP-IDF 固件工程
│   └── Scripts/           Windows 烧录脚本
├── Frontend/              Vue 3 + Vite 控制台
└── Backend/               FastAPI + SQLite + gmqtt 服务端
```

建议阅读顺序：

1. 先看本文，理解三部分如何配合
2. 再看 `Backend/README.md`，确认 API、MQTT 和环境变量
3. 再看 `Frontend/README.md`，确认控制台构建与挂载路径
4. 再看 `Hardware/README.md`，确认硬件端整体结构
5. 再看 `Hardware/Firmware/README.md`，确认固件编译、凭证和运行模型
6. 最后看 `Hardware/Firmware/components/README.md` 与 `Hardware/Firmware/components/sensor/README.md`，进入固件组件层

---

## 2. 系统组成

BeaconOps 由三端构成：

### 2.1 Hardware

硬件端基于 ESP32-C3，运行 ESP-IDF v5.x 固件，负责：

- 保留 PCB 工程源文件
- 保留外壳 STL / 3DM 模型
- 连接 Wi-Fi 与 MQTT Broker
- 接收下行命令并在本地 UI / 音频链路中呈现
- 上报在线状态、健康信息、事件与 profile
- 使用批次凭证生成 HMAC 鉴权密码

硬件相关详细说明见：

- `Hardware/README.md`
- `Hardware/PCB/README.md`
- `Hardware/Enclosure/README.md`
- `Hardware/Firmware/README.md`
- `Hardware/Scripts/README.md`
- `Hardware/Firmware/components/README.md`
- `Hardware/Firmware/components/sensor/README.md`

### 2.2 Backend

后端基于 FastAPI + SQLite + gmqtt，负责：

- 管理员登录与 Session / CSRF
- 批次、设备、消息、管理员、审计日志管理
- 作为 MQTT bridge 接收设备上行并下发消息
- 校验设备 MQTT 接入鉴权
- 提供前端控制台使用的 REST API 与事件流

后端详细说明见：

- `Backend/README.md`

### 2.3 Frontend

前端基于 Vue 3 + Vite + TypeScript，负责：

- 管理员登录
- 设备、批次、消息与审计视图
- 实时事件展示
- 控制台主题与交互界面

前端详细说明见：

- `Frontend/README.md`

---

## 3. 运行关系

系统运行链路可以概括为：

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

更具体地说：

1. 固件通过 `BROKER_URI` 连接 MQTT Broker。
2. 设备 MQTT 用户名使用 `BATCH_UUID`，密码由设备本地按 HMAC 规则动态生成。
3. Backend 以 bridge 账号连接 Broker，订阅设备上行主题，写入数据库，并为 Frontend 提供查询接口。
4. Frontend 通过 `/beacon/api/v1` 调用 Backend，并通过事件流获得实时更新。
5. 下行消息由 Frontend 发到 Backend，再由 Backend 经 Broker 发布到设备主题。

---

## 4. 三部分的职责边界

### 4.1 Hardware 与 Backend 的边界

Hardware 不保存管理员账号，也不直接暴露 HTTP 服务。

Hardware 需要本地配置的只有：

- `BATCH_UUID`
- `BATCH_SECRET`
- `WIFI_CRED_LIST`
- `BROKER_URI`

批次密钥只用于设备鉴权。批次的创建、撤销、轮换、设备分配、消息记录等都由 Backend 维护。

### 4.2 Frontend 与 Backend 的边界

Frontend 不内嵌真实账号、密码或 Token。

Frontend 依赖 Backend：

- 登录接口
- 批次、设备、消息、审计等 API
- Cookie Session
- CSRF Cookie / Header
- 事件流

Frontend 自身只负责 UI 与交互，不承担认证状态的最终可信存储。

### 4.3 部署边界

开源目录中不包含与你个人服务器绑定的部署目录、证书、数据库或日志。项目只约束以下外部契约：

- Frontend 对外路径：`/beacon/console/`
- Backend API 前缀：`/beacon/api/v1`
- Backend 健康检查：`/beacon/health`
- 设备 Broker 入口：`mqtts://YOUR_BROKER_HOST:8883`

至于是否使用 nginx、Caddy、Traefik、Broker 原生 TLS、Docker、systemd 或其它编排方式，由部署者自行决定。

---

## 5. 本地开发

### 5.1 Backend

进入 `Backend/` 后：

```bash
pip install -r requirements.txt
copy .env.example .env
python -m uvicorn app.main:app --host 127.0.0.1 --port 8020
```

你需要在 `.env` 中自行填写：

- `ADMIN_USERNAME`
- `ADMIN_PASSWORD_HASH`
- `JWT_SECRET`
- `MQTT_HOST`
- `MQTT_PORT`
- `MQTT_USERNAME`
- `MQTT_PASSWORD`
- `AUTH_WEBHOOK_TOKEN`（可选）

### 5.2 Frontend

进入 `Frontend/` 后：

```bash
npm install
npm run dev
```

开发环境下，前端会把 `/beacon/api/*` 代理到本地 Backend：

```text
http://127.0.0.1:8020
```

如果只在本机开发时需要临时跳过登录，可使用：

```text
.env.development.local
```

并设置：

```env
VITE_BYPASS_LOGIN=true
```

### 5.3 Hardware

Hardware 的构建流程是混合式：

- 在 WSL 内编译 ESP-IDF 固件
- 在 Windows 下执行烧录脚本

进入 `Hardware/Firmware/` 前，需要先在本地填好 `components/config/config.h` 中的占位符，再按 `Hardware/README.md` 中的流程编译与烧录。

---

## 6. 构建产物

### 6.1 Frontend 构建

在 `Frontend/` 中：

```bash
npm run build
```

默认构建结果是静态站点，面向 `/beacon/console/` 路径发布。

### 6.2 Hardware 构建

在 WSL 中进入 `Hardware/Firmware/`：

```bash
bash build.sh <你的Windows用户名>
```

脚本会编译固件并把合并后的 `main.bin` 复制到 Windows 桌面目录，供 `Hardware/Scripts/flash.bat` 烧录。

### 6.3 Backend 运行

Backend 本身不是前端那种“构建后产物”模型，而是直接运行 Python 服务。

如果希望由 Backend 同时托管前端静态文件，可将 Frontend 的构建结果放入：

```text
Backend/console/
```

`Backend/app/main.py` 会在该目录存在时自动暴露 `/beacon/console/`。

---

## 7. 通用部署方案

这里给出的是一套**项目级通用部署约定**，不是某一台服务器的目录结构。

### 7.1 最小可运行拓扑

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

### 7.2 生产环境需要满足的条件

1. 公开一个给设备访问的 Broker 地址，例如 `YOUR_BROKER_HOST:8883`。
2. 为该入口配置 TLS。
3. 启动 Backend，并配置可用的 bridge 账号与 JWT 密钥。
4. 构建 Frontend，并把它发布到 `/beacon/console/`。
5. 让 `/beacon/api/v1` 指向 Backend。
6. 让 `/beacon/health` 指向 Backend 健康接口。
7. 在固件中把 `BROKER_URI` 改为生产 Broker 地址，并重新编译烧录。

### 7.3 推荐的部署顺序

1. 先部署 MQTT Broker，并验证 Backend 能连接。
2. 再启动 Backend，确认 `/beacon/health` 正常。
3. 再构建并发布 Frontend。
4. 最后写入固件凭证并烧录设备。

### 7.4 路由契约

无论具体使用什么 Web 服务器，建议都保持以下路径不变：

- `/beacon/console/`：前端控制台
- `/beacon/api/v1/*`：后端 API
- `/beacon/health`：健康检查

这样可以保证 Frontend、Backend 与文档中的默认配置保持一致，减少二次改造。

---

## 8. 硬件部署注意事项

硬件端不是简单“刷入即用”，还需要满足以下前置条件：

1. 已准备可用的批次凭证。
2. 已准备可连接的 Wi-Fi 列表。
3. 已准备 Broker 的 TLS 入口地址。
4. 如启用自定义 CA 文件，需要在本地补齐证书材料。

固件开源目录中的默认值都是占位符，不包含真实 Wi-Fi、批次密钥或生产 Broker 地址。

---

## 9. 安全与敏感信息

开源目录中已经移除了以下内容：

- 真实 `.env` / `.env.local`
- 后端数据库、日志与运行缓存
- 固件中的真实批次密钥、Wi-Fi 凭证和 Broker 地址
- 前端本地开发环境文件
- 证书文件与本机运维目录

你在本地部署时需要自行准备：

- 管理员密码哈希
- JWT 密钥
- MQTT bridge 账号密码
- 设备批次凭证
- Wi-Fi 凭证
- Broker TLS 配置

---

## 10. 进一步阅读

以下为 BeaconOps 自己维护的项目文档。`Hardware/Firmware/components/display/` 下的 README / CHANGELOG 属于上游第三方文档，随源码保留，不作为本项目文档集的一部分整理。

- `Hardware/README.md`
- `Hardware/PCB/README.md`
- `Hardware/Enclosure/README.md`
- `Hardware/Firmware/README.md`
- `Hardware/Scripts/README.md`
- `Hardware/Firmware/components/README.md`
- `Hardware/Firmware/components/sensor/README.md`
- `Frontend/README.md`
- `Backend/README.md`

如果你只是想快速启动系统，优先看：

1. `Backend/README.md`
2. `Frontend/README.md`
3. `Hardware/README.md`

如果你要准备真实设备部署，则再继续看：

1. `Hardware/Firmware/README.md`
2. `Hardware/Scripts/README.md`