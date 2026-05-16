# BeaconOps 数据模型 v1

> 状态:**权威**(实现以本文档为准)
> 配套:[protocol-v1.md](./protocol-v1.md)、[decisions.md](./decisions.md)
> 实施策略:**删表重建**,不做迁移(用户原话"几乎要重做")。

---

## 0. 总览

```
                          ┌──────────────┐
                          │   batches    │  授权白名单(批次)
                          └──────┬───────┘
                                 │ 1:N
                          ┌──────▼───────┐
                          │   devices    │  物理终端(MAC = 主键)
                          └──────┬───────┘
                                 │ 1:N
       ┌─────────────────────────┼────────────────────────────┐
       │                         │                            │
┌──────▼──────┐          ┌───────▼────────┐         ┌─────────▼────────┐
│  messages   │          │ device_health  │         │     behavior     │
│ (9 状态机)   │          │ _history(可选) │         │ (EC-BAID 决策记录)│
└──────┬──────┘          └────────────────┘         └──────────────────┘
       │
       │ 1:N
┌──────▼───────┐         ┌──────────────────┐
│ message_     │         │   admins         │ 操作员账号(独立体系)
│  events      │         └────────┬─────────┘
│ (ack + 端事件)│                  │
└──────────────┘         ┌────────▼─────────┐
                         │   audit_log      │
                         └──────────────────┘

                         ┌──────────────────┐
                         │ scheduled_tasks  │ 定时/延时下发
                         └──────────────────┘
                         ┌──────────────────┐
                         │ confirm_tokens   │ LLM 高危操作二次确认
                         └──────────────────┘
                         ┌──────────────────┐
                         │ mqtt_nonces      │ 重放窗口去重 (TTL)
                         └──────────────────┘
```

**删表清单**(以前有,现在删):

- `users` / `user_roles`
- `holders`(若存在的 view/表)
- `telemetry`(老 schema,被 health/profile 取代)
- `tts_*` 任何表/缓存
- `profile`(若是 holder profile)

---

## 1. 表定义(SQLite)

### 1.1 `batches` — 批次许可白名单

```sql
CREATE TABLE batches (
    batch_uuid       TEXT PRIMARY KEY,           -- 自由格式,如 'b2026q2-0001'
    alias            TEXT NOT NULL DEFAULT '',   -- 显示名
    batch_secret     TEXT NOT NULL,              -- HMAC 密钥(64 hex / 任意长度)
    produced_at      INTEGER NOT NULL,           -- 出厂时间 unix 秒
    produced_count   INTEGER NOT NULL DEFAULT 0, -- 该批生产数量(目标值)
    revoked          INTEGER NOT NULL DEFAULT 0, -- 0/1
    revoked_at       INTEGER,
    revoked_reason   TEXT NOT NULL DEFAULT '',
    notes            TEXT NOT NULL DEFAULT '',
    created_at       INTEGER NOT NULL,
    updated_at       INTEGER NOT NULL
);
CREATE INDEX idx_batches_revoked ON batches(revoked);
```

### 1.2 `devices` — 物理终端

```sql
CREATE TABLE devices (
    device_id        TEXT PRIMARY KEY,           -- 12 位小写 hex (MAC)
    batch_uuid       TEXT NOT NULL,              -- FK → batches.batch_uuid
    alias            TEXT NOT NULL DEFAULT '',   -- 自由备注(常写"谁在用")
    dept             TEXT NOT NULL DEFAULT '',   -- 单值,空字符串=未分配
    roles_json       TEXT NOT NULL DEFAULT '[]', -- JSON 数组,如 ["medic","leader"]
    enabled          INTEGER NOT NULL DEFAULT 1, -- 单台禁用开关

    -- 来自 status (LWT)
    online           INTEGER NOT NULL DEFAULT 0,
    fw_version       TEXT NOT NULL DEFAULT '',

    -- 来自 health
    battery_soc      INTEGER NOT NULL DEFAULT -1,    -- 0..100, -1=未知
    charging         INTEGER NOT NULL DEFAULT 0,
    rssi             INTEGER NOT NULL DEFAULT 0,
    last_ip          TEXT NOT NULL DEFAULT '',
    uptime_s         INTEGER NOT NULL DEFAULT 0,
    spiffs_pending   INTEGER NOT NULL DEFAULT 0,
    ack_pending      INTEGER NOT NULL DEFAULT 0,
    drop_count       INTEGER NOT NULL DEFAULT 0,

    first_seen_at    INTEGER,
    last_seen_at     INTEGER,
    last_health_at   INTEGER,

    created_at       INTEGER NOT NULL,
    updated_at       INTEGER NOT NULL,

    FOREIGN KEY (batch_uuid) REFERENCES batches(batch_uuid)
);
CREATE INDEX idx_devices_batch    ON devices(batch_uuid);
CREATE INDEX idx_devices_dept     ON devices(dept);
CREATE INDEX idx_devices_online   ON devices(online);
CREATE INDEX idx_devices_enabled  ON devices(enabled);
```

> `roles_json` 用 JSON 字符串避免 N:M 中间表;查询时用 `json_each(roles_json)`(SQLite 内建 JSON1)。

### 1.3 `messages` — 消息主体(9 状态机)

```sql
CREATE TABLE messages (
    msg_id           TEXT PRIMARY KEY,           -- 32 hex (服务器生成)

    -- 目标(单播 / 广播二选一)
    target_kind      TEXT NOT NULL,              -- 'device' | 'all' | 'dept' | 'role'
    target_value     TEXT NOT NULL DEFAULT '',   -- device_id | '' | dept | role
                                                 --   广播展开时按这字段实时查 devices 表

    -- 内容(协议字段 §3.1)
    level            TEXT NOT NULL,              -- info|notice|warn|emergency
    title            TEXT NOT NULL DEFAULT '',
    body             TEXT NOT NULL DEFAULT '',
    ttl              INTEGER NOT NULL DEFAULT 0,
    ack_mode         TEXT NOT NULL,              -- none|received|displayed|acknowledged

    -- 来源
    source_type      TEXT NOT NULL,              -- 'admin' | 'llm' | 'scheduled' | 'system'
    operator         TEXT NOT NULL DEFAULT '',   -- admin.username 或 'llm:<session>'

    -- 状态
    status           TEXT NOT NULL,              -- queued|sent|delivered|displayed|acknowledged|expired|failed
    attempt_count    INTEGER NOT NULL DEFAULT 0,
    next_retry_at    INTEGER,
    last_send_at     INTEGER,
    last_error       TEXT NOT NULL DEFAULT '',

    -- 时间
    created_at       INTEGER NOT NULL,
    sent_at          INTEGER,
    delivered_at     INTEGER,                    -- received ack 到的时刻
    displayed_at     INTEGER,
    acknowledged_at  INTEGER,
    expired_at       INTEGER,
    failed_at        INTEGER,

    payload_json     TEXT NOT NULL               -- 实际下发 JSON(便于审计/回放)
);
CREATE INDEX idx_messages_status     ON messages(status);
CREATE INDEX idx_messages_target     ON messages(target_kind, target_value);
CREATE INDEX idx_messages_created    ON messages(created_at);
CREATE INDEX idx_messages_next_retry ON messages(next_retry_at) WHERE status IN ('queued','sent');
```

**广播展开规则**:
- `target_kind='all'`:发布到 `broadcast/all/cmd`,服务器在 publish 同时为**每个在线设备**创建一行 `message_recipients`(见 1.4)。
- 同理 `dept` / `role`。
- 单播 `target_kind='device'`:直接发 `device/{id}/cmd`,**不写** `message_recipients`(可选,看 §1.4 设计取舍)。

### 1.4 `message_recipients` — 广播展开后的逐设备追踪(可选但推荐)

```sql
CREATE TABLE message_recipients (
    msg_id           TEXT NOT NULL,
    device_id        TEXT NOT NULL,
    status           TEXT NOT NULL DEFAULT 'queued',  -- 同 messages.status 9 状态
    delivered_at     INTEGER,
    displayed_at     INTEGER,
    acknowledged_at  INTEGER,
    expired_at       INTEGER,
    failed_at        INTEGER,
    last_error       TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (msg_id, device_id),
    FOREIGN KEY (msg_id)    REFERENCES messages(msg_id),
    FOREIGN KEY (device_id) REFERENCES devices(device_id)
);
CREATE INDEX idx_recipients_device ON message_recipients(device_id);
CREATE INDEX idx_recipients_status ON message_recipients(status);
```

> 单播时也写一行,统一 `messages.status` 仅作"聚合视图",真实状态以 `message_recipients` 为准。前端"消息详情"页直接渲染该表。

### 1.5 `message_events` — 端事件 + ack 事件流水(取代旧 `acks` 表)

```sql
CREATE TABLE message_events (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    msg_id           TEXT NOT NULL,
    device_id        TEXT NOT NULL,              -- 哪台报的
    kind             TEXT NOT NULL,
                     -- ack 类:received | displayed | acknowledged | expired
                     -- event 类:ack_give_up | parse_reject | store_full | dedup_drop
                     -- server 类:publish | retry | retry_exhausted
    detail           TEXT NOT NULL DEFAULT '',   -- 自由文本(error/reason)
    ts               INTEGER NOT NULL,           -- 事件时间(端上行带,server 类用本地)
    recorded_at      INTEGER NOT NULL,
    FOREIGN KEY (msg_id)    REFERENCES messages(msg_id),
    FOREIGN KEY (device_id) REFERENCES devices(device_id)
);
CREATE INDEX idx_events_msg     ON message_events(msg_id);
CREATE INDEX idx_events_device  ON message_events(device_id);
CREATE INDEX idx_events_kind    ON message_events(kind);
CREATE UNIQUE INDEX uq_events_dedup ON message_events(msg_id, device_id, kind);
-- ↑ 同设备同消息同 kind 只能有一行(防止 ack 重传/重复入库)
```

### 1.6 `device_health_history` — 健康时序(可选,默认关闭)

```sql
CREATE TABLE device_health_history (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id        TEXT NOT NULL,
    ts               INTEGER NOT NULL,
    battery_soc      INTEGER,
    charging         INTEGER,
    rssi             INTEGER,
    spiffs_pending   INTEGER,
    ack_pending      INTEGER,
    FOREIGN KEY (device_id) REFERENCES devices(device_id)
);
CREATE INDEX idx_health_device_ts ON device_health_history(device_id, ts);
```

> 默认 `BEACON_HEALTH_HISTORY=0`,只 UPSERT 到 `devices`;开启后追加一行。

### 1.7 `admins` — 操作员账号(独立于设备)

```sql
CREATE TABLE admins (
    username         TEXT PRIMARY KEY,
    password_hash    TEXT NOT NULL,              -- argon2id 或 bcrypt
    display_name     TEXT NOT NULL DEFAULT '',
    role             TEXT NOT NULL DEFAULT 'operator',  -- admin | operator | viewer
    enabled          INTEGER NOT NULL DEFAULT 1,
    created_at       INTEGER NOT NULL,
    last_login_at    INTEGER
);
```

### 1.8 `audit_log` — 后台审计

```sql
CREATE TABLE audit_log (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    ts               INTEGER NOT NULL,
    actor_type       TEXT NOT NULL,              -- 'admin' | 'llm' | 'system'
    actor            TEXT NOT NULL,              -- username 或 'llm:<session>'
    action           TEXT NOT NULL,              -- 'send_message' | 'revoke_batch' | ...
    target_kind      TEXT NOT NULL DEFAULT '',
    target_value     TEXT NOT NULL DEFAULT '',
    detail_json      TEXT NOT NULL DEFAULT '{}',
    ip               TEXT NOT NULL DEFAULT ''
);
CREATE INDEX idx_audit_ts     ON audit_log(ts);
CREATE INDEX idx_audit_actor  ON audit_log(actor);
CREATE INDEX idx_audit_action ON audit_log(action);
```

### 1.9 `scheduled_tasks` — 延时/定时下发

```sql
CREATE TABLE scheduled_tasks (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    fire_at          INTEGER NOT NULL,           -- 触发时间 unix 秒
    status           TEXT NOT NULL DEFAULT 'pending',  -- pending|fired|cancelled|failed
    target_kind      TEXT NOT NULL,
    target_value     TEXT NOT NULL DEFAULT '',
    payload_json     TEXT NOT NULL,              -- 与 messages 同 schema
    created_by       TEXT NOT NULL,
    created_at       INTEGER NOT NULL,
    fired_at         INTEGER,
    msg_id           TEXT,                       -- fired 后回填
    last_error       TEXT NOT NULL DEFAULT ''
);
CREATE INDEX idx_sched_fire ON scheduled_tasks(fire_at) WHERE status='pending';
```

### 1.10 `confirm_tokens` — LLM 高危操作二次确认

```sql
CREATE TABLE confirm_tokens (
    token            TEXT PRIMARY KEY,           -- 32 hex
    action           TEXT NOT NULL,              -- 'send_emergency' | 'revoke_batch' | ...
    payload_json     TEXT NOT NULL,
    issued_to        TEXT NOT NULL,              -- 'llm:<session>'
    issued_at        INTEGER NOT NULL,
    expires_at       INTEGER NOT NULL,           -- ≤ 5 分钟
    consumed_at      INTEGER
);
CREATE INDEX idx_confirm_expires ON confirm_tokens(expires_at);
```

### 1.11 `mqtt_nonces` — HMAC 重放窗口

```sql
CREATE TABLE mqtt_nonces (
    nonce            TEXT PRIMARY KEY,
    device_id        TEXT NOT NULL,
    seen_at          INTEGER NOT NULL,
    expires_at       INTEGER NOT NULL            -- seen_at + 600
);
CREATE INDEX idx_nonces_expires ON mqtt_nonces(expires_at);
```

> 生产可改 Redis;落地 v1 用 SQLite 简化部署。后台 task 每分钟清 expired。

### 1.12 `behavior` / `behavior_decision` / `profile_7d`

保留**现状结构**,只把外键由 `device_uuid` 改为 `device_id`(命名统一)。本期不动算法。

---

## 2. REST 接口清单

> 全部前缀 `/api/v1`,认证除 `/auth/login` 和 `/auth/device` 外都要 admin session cookie 或 bearer。

### 2.1 认证 / 设备接入

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/auth/login`         | 操作员登录,返回 cookie |
| POST | `/auth/logout`        | 注销 |
| GET  | `/auth/me`            | 当前操作员信息 |
| POST | `/auth/device`        | **MQTT broker auth webhook**,入参 `{client_id, username, password}`,出参 ACL |

### 2.2 批次

| 方法 | 路径 | 说明 |
|---|---|---|
| GET    | `/batches`                       | 列表 |
| POST   | `/batches`                       | 创建(自动生成 batch_secret) |
| GET    | `/batches/{batch_uuid}`          | 详情(含设备数统计) |
| PATCH  | `/batches/{batch_uuid}`          | 改 alias/notes/produced_count |
| POST   | `/batches/{batch_uuid}/revoke`   | 撤销(并 disconnect 该批所有在线设备) |
| POST   | `/batches/{batch_uuid}/secret`   | 轮换 batch_secret(影响下次接入) |

### 2.3 设备

| 方法 | 路径 | 说明 |
|---|---|---|
| GET    | `/devices`                                   | 列表 + 筛选 `?batch=&dept=&role=&online=&q=`(alias 模糊) |
| GET    | `/devices/{device_id}`                       | 详情 |
| PATCH  | `/devices/{device_id}`                       | 改 alias/dept/roles_json/enabled |
| POST   | `/devices/{device_id}/disconnect`            | 主动踢下线(强制重连重读 ACL) |
| GET    | `/devices/{device_id}/messages`              | 该设备消息历史 |
| GET    | `/devices/{device_id}/events`                | 该设备 message_events 流水 |
| GET    | `/devices/{device_id}/health/history`        | 健康时序(若开启) |

### 2.4 消息

| 方法 | 路径 | 说明 |
|---|---|---|
| POST   | `/messages`                                  | 创建并立即下发,入参 `{target_kind, target_value, level, title, body, ttl, ack_mode}` |
| GET    | `/messages`                                  | 列表 + 筛选 `?status=&level=&from=&to=&target=` |
| GET    | `/messages/{msg_id}`                         | 详情 |
| GET    | `/messages/{msg_id}/recipients`              | 广播展开后各设备状态 |
| GET    | `/messages/{msg_id}/events`                  | 该消息全部事件流水 |
| POST   | `/messages/{msg_id}/cancel`                  | 仅 queued/sent 状态可取消(停重试) |
| POST   | `/messages/{msg_id}/resend`                  | 失败/过期消息整条复制重发 |

### 2.5 定时任务

| 方法 | 路径 | 说明 |
|---|---|---|
| GET    | `/scheduled`                          | 列表 |
| POST   | `/scheduled`                          | 创建 |
| DELETE | `/scheduled/{id}`                     | 取消(若 pending) |

### 2.6 LLM / MCP

| 方法 | 路径 | 说明 |
|---|---|---|
| POST   | `/llm/chat`                       | 流式对话(SSE),返回 token + tool_call 事件 |
| POST   | `/llm/confirm/{token}`            | 二次确认高危操作 |
| GET    | `/llm/sessions`                   | 当前操作员的会话列表 |

> MCP tools 子集:`list_devices / send_message / query_message / cancel_message`(无 `query_user`)。

### 2.7 操作员

| 方法 | 路径 | 说明 |
|---|---|---|
| GET    | `/admins`            | 列表 (role=admin 才能看) |
| POST   | `/admins`            | 创建 |
| PATCH  | `/admins/{username}` | 改 role/enabled/password |
| DELETE | `/admins/{username}` | 删除(不能删自己) |

### 2.8 审计

| 方法 | 路径 | 说明 |
|---|---|---|
| GET    | `/audit`                          | 列表 + 筛选 `?actor=&action=&from=&to=` |

### 2.9 实时流

| 方法 | 路径 | 说明 |
|---|---|---|
| GET    | `/stream`                         | SSE,推送事件:`device.online/offline/health`,`message.status_change`,`message.event` |

---

## 3. SSE 事件 schema

```jsonc
// device.health
{ "type": "device.health", "device_id": "...", "battery_soc": 78, "rssi": -62, "ts": ... }

// device.status
{ "type": "device.online",  "device_id": "...", "fw": "...", "ts": ... }
{ "type": "device.offline", "device_id": "...", "ts": ... }

// message.status_change
{ "type": "message.status_change", "msg_id": "...", "device_id": "..."(广播时), 
  "from": "sent", "to": "delivered", "ts": ... }

// message.event
{ "type": "message.event", "msg_id": "...", "device_id": "...", 
  "kind": "parse_reject", "detail": "...", "ts": ... }
```

---

## 4. 实施清单

- [ ] 删 `users / user_roles / holders / telemetry / tts_*` 旧表
- [ ] 建上面 12 张表
- [ ] `devices.uuid` → `device_id` 命名统一
- [ ] `acks` 表合并入 `message_events`
- [ ] 加 `messages.attempt_count / next_retry_at / last_error` 列
- [ ] 加 `message_recipients` 表用于广播追踪
- [ ] 改 `behavior.*` 表中 `device_uuid` 列为 `device_id`

---

**文档结束**
