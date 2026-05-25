# Backend

BeaconOps 后端，FastAPI + SQLite + gmqtt，一个进程里同时讲两门语言：对前端说 HTTP，对设备说 MQTT。职责包括管理员登录、批次与设备管理、消息下发、MQTT 上下行桥接、审计日志和实时事件流。

SQLite 是有意选的——照这个项目的并发量根本用不上 Postgres，外加依赖也额外多一个要运维的东西。

## 技术栈

- Python 3.11
- FastAPI
- aiosqlite
- gmqtt
- PyJWT
- bcrypt

## 目录结构

```text
Backend/
├── app/
│   ├── main.py           FastAPI 入口
│   ├── config.py         环境变量与路径配置
│   ├── database.py       SQLite schema 与持久层
│   ├── mqtt_client.py    服务端 MQTT bridge
│   ├── identity.py       设备 HMAC 鉴权
│   ├── messages.py       下行消息与重试调度
│   ├── routes/           HTTP API 路由
│   └── ...
├── .env.example
├── requirements.txt
└── README.md
```

## API 与路由前缀

服务端 API 前缀固定为：

```text
/beacon/api/v1
```

健康检查接口：

```text
/beacon/health
```

主要路由分组：

- auth：管理员登录（Cookie + CSRF 双重保护，`csrf_guard` 中间件）、登出、当前会话、设备接入鉴权 webhook
- batches：批次创建、查看、撤销（撤销后异步重启 Mosquitto broker，主动踢掉当前在线设备）、恢复、轮换密钥
- devices：设备列表与设备详情
- messages：消息发送、查询、回执查看
- admins：管理员账户管理
- audit：审计日志（支持按操作人、操作类型、时间范围过滤）
- stream：实时事件流（消息状态变化、设备 health、设备行为三路 SSE）

## 数据与运行方式

数据库默认写入 `DATA_DIR`，默认位置是：

```text
./data/beacon.db
```

应用启动时会自动建表，并在配置了 `ADMIN_USERNAME` 与 `ADMIN_PASSWORD_HASH` 时写入一个种子管理员账号。

批次密钥 `batch_secret` 不是写死在源码中的常量，而是在创建批次或轮换密钥时由后端运行期生成，并存入数据库。

下行消息的重试不阻塞 publish 主流程：`messages.py` 里的 `retry_loop` 按 `next_retry_at` 异步调度重试。设备重新上线时（`uplink_handler` 收到 `status=online`）会触发 `on_device_online`，立刻把当前 queued / sent 状态的消息补推一次，不需要等下一个重试周期。

## 与 MQTT Broker 的关系

后端会使用 `MQTT_USERNAME` / `MQTT_PASSWORD` 以 bridge 身份连接 Broker，并订阅以下上行主题：

- `device/+/status`
- `device/+/uplink/ack`
- `device/+/uplink/event`
- `device/+/uplink/health`
- `device/+/uplink/profile`

设备鉴权接口是：

```text
POST /beacon/api/v1/auth/device
```

未配置 `AUTH_WEBHOOK_TOKEN` 时，仅允许本机直连该 webhook。

## 本地开发

安装依赖：

```bash
pip install -r requirements.txt
```

复制环境文件：

```bash
copy .env.example .env
```

启动服务：

```bash
python -m uvicorn app.main:app --host 127.0.0.1 --port 8020
```

如果需要让后端同时托管前端构建产物，请把前端构建结果放到 `Backend/console/` 目录。`app/main.py` 会在该目录存在时自动暴露 `/beacon/console/`。

## 开源版去除了什么

当前开源目录未包含以下本地内容：

- `.env`
- `.env.local`
- `data/` 中的数据库与日志
- Python 虚拟环境与缓存目录

你需要自行提供管理员密码哈希、JWT 密钥、MQTT bridge 凭据以及可选的 webhook token。