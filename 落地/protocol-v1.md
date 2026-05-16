# BeaconOps 通信协议规格(落地 v1)

> 状态:**权威**(实现以本文档为准,与 `论文/01-通信协议规格.md` 不一致时以本文为准)
> 适用:固件 / 服务器 / 控制台 三端
> 版本:v1.0(2026-05-15 落地)
> 关联:`decisions.md`(列出与论文规格的所有偏差及理由)

---

## 0. 设计原则(必须遵守)

1. **协议是唯一事实源**:固件、服务器、前端只准实现本文档定义的字段与 topic,任何"兼容旧字段"代码必须删除。
2. **消息为中心,设备为载体**:所有业务对象围绕 `device` 与 `message` 展开,不存在 `holder / user / 持有人` 的领域概念(参见 `decisions.md` D-02)。
3. **MAC = 设备身份,batch_uuid = 接入许可**(D-01)。
4. **无 TTS、无音频流**(D-03,呼应 TODO)。
5. **每个状态点都可观测**:消息 9 状态全部回到服务器并向前端暴露(D-04)。

---

## 1. 身份与白名单

### 1.1 名词定义

| 名词 | 含义 | 唯一性 | 来源 |
|---|---|---|---|
| `device_id` | 物理设备身份,12 位小写 hex MAC | 全球唯一 | `esp_efuse_mac_get_default()` 运行时读取,**不烧录** |
| `batch_uuid` | 批次许可证,服务器侧白名单的主键 | 1 个 UUID 对应 N 台同批设备 | **`config.h` 编译时常量** `BATCH_UUID`,与主固件一块烧 |
| `batch_secret` | 批次共享 HMAC 密钥 | 与 `batch_uuid` 1:1 | **`config.h` 编译时常量** `BATCH_SECRET`,设备永不上行 |
| `admin` | 控制台操作员账号 | 全局唯一 | 服务器 DB,与设备无关 |

> `device_id` 示例:`a1b2c3d4e5f6`(MAC `A1:B2:C3:D4:E5:F6`,去冒号小写)。
> `batch_uuid` 示例:`b2026q2-0001`(自由格式字符串,长度 ≤ 64)。

### 1.2 接入鉴权

```
固件 MQTT CONNECT:
  client_id = device_id
  username  = batch_uuid
  password  = HMAC_SHA256(batch_secret, device_id || "|" || ts || "|" || nonce)
  其中 ts = 当前 unix 秒(SNTP 已同步),nonce = 16 字节随机 hex

服务器(broker auth webhook 或同进程 auth 模块):
  1. SELECT * FROM batches WHERE batch_uuid=? AND revoked=0     —— 不通过 → 拒
  2. abs(server_now - ts) <= 300                                 —— 不通过 → 拒
  3. nonce 在 Redis(或内存 LRU)未出现过(TTL 600 s)           —— 不通过 → 拒
  4. HMAC 校验通过                                               —— 不通过 → 拒
  5. UPSERT devices(device_id, batch_uuid, last_seen=now, ...)
  6. 返回 ACL:
       allow sub  device/{device_id}/cmd
       allow sub  broadcast/all/cmd
       allow sub  broadcast/dept/{device.dept}/cmd     (若已配置 dept)
       allow sub  broadcast/role/{r}/cmd               (每个 role 一行)
       allow pub  device/{device_id}/uplink/+
       allow pub  device/{device_id}/status
       deny  其它
```

**撤销语义**:
- `UPDATE batches SET revoked=1 WHERE batch_uuid=?` → 该批所有设备**下次重连被拒**;当前实现不主动踢掉已连接会话,现有连接会保持到其自然断线或 broker/运维侧手动断开。
- `UPDATE devices SET enabled=0 WHERE device_id=?` → 单台禁用,正交于批次撤销。

### 1.3 烧录与 Wi-Fi 接入(对应 TODO §9.1)

**设计原则**:企业场景下员工不可能知道内网 Wi-Fi 密码,设备出厂时一次烧入全部凭证,现场开机即用,无现场配网环节。

| 项 | 内容 | 位置 | 怎么烧 |
|---|---|---|---|
| 主固件 | bootloader / partition / app | flash | `idf.py flash` |
| 批次凭证 | `BATCH_UUID` + `BATCH_SECRET` | **`components/config/config.h`(编译时常量)** | 随主固件同烧;生产脚本可生成 `batch_credentials.h` 覆盖 `#define` |
| Wi-Fi SSID 列表 | 一个或多个 (ssid, psk) | **`components/config/config.h` `WIFI_CRED_LIST`** | 随主固件同烧 |
| Broker 地址 | `mqtts://cocandy.com.cn:8883` | **`components/config/config.h` `BROKER_URI`** | 随主固件同烧;CA 默认用 `certs.c` 内置的 ISRG Root X1(Let's Encrypt) |

**传输层安全**:设备 ↔ `cocandy.com.cn:8883` 走 TLS 1.2/1.3;TLS 由服务器 nginx `stream` 块终止,代理到本机 `127.0.0.1:1883` 的 Mosquitto(不对公网暴露)。证书复用 `cocandy.com.cn` 的 Let's Encrypt,由 80 端口 acme-challenge 续签。配置位于 `Server/services/beacon/scripts/nginx.stream.conf`,由 `/etc/nginx/nginx.conf` 顶层 `include /opt/server/services/*/scripts/nginx.stream.conf;` 收入。

**多 SSID 连接策略**:设备上电后主动扫描周边 AP,与 `WIFI_CRED_LIST` 交集的 SSID 中选信号最强者连接;连接失败/掉线后重扫重选。最后一次连上的 SSID 会写入 NVS `wifi/last_good_ssid`,下次启动优先尝试。

**切批次**:重新编译固件(或产线重生成 `batch_credentials.h`)后重烧 app 分区即可,无需 NVS 操作。

---

## 2. MQTT Topic 全集

> `{id}` 一律指 `device_id`(MAC 12 hex)。所有 topic 之外**禁止**新增。

| 方向 | Topic | QoS | retain | 说明 |
|---|---|---|---|---|
| 下行 | `device/{id}/cmd` | 1 | false | 单条消息卡片 |
| 下行 | `broadcast/all/cmd` | 1 | false | 全员广播 |
| 下行 | `broadcast/dept/{dept}/cmd` | 1 | false | 部门广播 |
| 下行 | `broadcast/role/{role}/cmd` | 1 | false | 角色广播 |
| 上行 | `device/{id}/status` | 1 | **true** | 在线/离线(LWT)+ 固件版本 |
| 上行 | `device/{id}/uplink/ack` | 1/2 | false | 4 类消息 ack |
| 上行 | `device/{id}/uplink/event` | 1 | false | 端侧事件(parser 拒收/ack 放弃/丢消息) |
| 上行 | `device/{id}/uplink/health` | 0 | false | 周期健康(电量/RSSI/IP/队列深度) |
| 上行 | `device/{id}/uplink/profile` | 1 | false | 60 s 行为窗口 |

**已删除**(论文有但落地不要):
- `device/{id}/audio`(D-03 移除 TTS)
- `device/{id}/uplink/hb`(被 health 替代)
- `device/{id}/uplink/telemetry`(旧 schema,被 profile 替代)
- `device/{id}/shake`(摇晃只作为 ack 触发器,不单独上行)
- `system/time`、`system/notice`(暂不需要)

---

## 3. 消息描述格式(MDL)

### 3.1 下行 cmd payload(权威 schema)

```jsonc
{
  "id":     "550e8400e29b41d4a716446655440000",   // 必填,32 位 hex(服务器分配)
  "ts":     1715587200,                            // 必填,unix 秒(服务器发送时刻)
  "ttl":    3600,                                  // 必填,秒;0 = 永不过期
  "level":  "info",                                // 必填,枚举 info|notice|warn|emergency
  "title":  "开会通知",                            // title/body 至少一个非空
  "body":   "下午3点到3楼会议室",
  "ack":    "acknowledged"                         // 必填,枚举 none|received|displayed|acknowledged
}
```

**字段约束**:

| 字段 | 类型 | 约束 |
|---|---|---|
| `id` | string | 32 位 hex,服务器侧用 `secrets.token_hex(16)` 生成,设备侧只用于去重 |
| `ts` | int | unix 秒,设备侧仅用作显示时间和 expire 计算 |
| `ttl` | int | ≥ 0;`level=emergency` 时设备侧强制视为 0(永不过期) |
| `level` | string | 仅接受 4 字符串,**禁止整数** |
| `title` / `body` | string | 至少一非空,否则服务器拒绝入库,固件 parser 拒收并发 `event(parse_reject)` |
| `ack` | string | 单字段递进语义见 §3.2 |

### 3.2 ack 模式(`ack` 字段语义)

| 取值 | 设备侧上报哪些 ack |
|---|---|
| `none` | 不发任何 ack(fire-and-forget,服务器仍记 sent) |
| `received` | 落盘后发 `received`;不发 displayed/acknowledged/expired |
| `displayed` | 落盘后发 `received`;首次上屏发 `displayed`;不强制摇一摇 |
| `acknowledged` | 全程 4 ack:received → displayed → (acknowledged \| expired) |

> `level=warn|emergency` 时服务器**强制** `ack=acknowledged`,LLM 工具入参不允许覆盖。

### 3.3 上行 ack payload

```jsonc
// device/{id}/uplink/ack
{
  "msg_id": "550e8400e29b41d4a716446655440000",
  "kind":   "received",          // received|displayed|acknowledged|expired
  "ts":     1715587201
}
```

> 删除论文中的 `via / device_id / type` 等冗余字段。`device_id` 已包含在 topic 中,`via` 暂无业务消费方,`type` 与 `kind` 同义。

### 3.4 上行 event payload(新增,填补 S9 黑洞)

```jsonc
// device/{id}/uplink/event
{
  "kind":   "ack_give_up",       // ack_give_up | parse_reject | store_full | dedup_drop
  "msg_id": "...",                // 与 kind 相关时必填,无关时空字符串
  "reason": "max_attempts=10",   // 自由文本,≤ 120 字符
  "ts":     1715587260
}
```

| `kind` | 触发 | 服务器副作用 |
|---|---|---|
| `ack_give_up` | tx 组件某 ack 重发 10 次未成功 | `messages.status='failed'`,`last_error="device_ack_give_up:{ack_kind}"` |
| `parse_reject` | parser 拒收一条下行 | `messages.status='failed'`,`last_error="device_parse_reject:{reason}"` |
| `store_full` | SPIFFS 消息槽位满,丢弃新到的 | `messages.status='failed'`,`last_error="device_store_full"` |
| `dedup_drop` | LRU 命中,设备视为重复消息直接丢 | 仅日志,不改 status(本就是服务器重发) |

### 3.5 上行 health payload(新增,取代 hb)

```jsonc
// device/{id}/uplink/health  (周期 30 s + 电量变化≥3% 或 RSSI 变化≥10dB 触发)
{
  "ts":             1715587260,
  "uptime_s":       3600,
  "battery":        78,           // 0..100,缺测 → -1
  "charging":       false,
  "rssi":           -62,
  "ip":             "192.168.1.50",
  "fw":             "BeaconOps-1.0",
  "spiffs_pending": 2,            // SPIFFS 内待显示/未 ack 消息数
  "ack_pending":    0,            // tx 组件 NVS 内待发 ack 数
  "drop_count":     0             // 启动以来 parser 累计 drop
}
```

**服务器处理**:`UPSERT devices(battery_soc, rssi, last_ip, fw_version, last_seen)`,可选追加一行到 `device_health_history`(时序,可关闭)。

### 3.6 上行 status payload(LWT)

```jsonc
// device/{id}/status   QoS=1, retain=true
// 在线时设备主动发布:
{ "online": true,  "fw": "BeaconOps-1.0", "ts": 1715587200 }
// 掉线时由 broker 自动发布(LWT 注册):
{ "online": false }
```

### 3.7 上行 profile payload(行为窗口,沿用现状)

```jsonc
// device/{id}/uplink/profile   每 60 s
{
  "ts":             1715587260,
  "win":            60,
  "static":         42,           // 秒
  "walk_slow":      10,
  "walk_fast":      5,
  "run":            0,
  "shake_or_fall":  3,
  "shake_n":        1,
  "intensity_avg":  2.4,
  "steps":          18            // 可缺,缺即 null
}
```

> 与论文 telemetry 的差异:删除 `seq / battery / rssi / queue.*`(已迁到 health),删除 `manual_ack_inc / auto_ack_inc`(从 acks 表统计)。

---

## 4. 消息 9 状态机

```
                                                     ┌─→ acknowledged   (terminal)
queued ──server publish──→ sent ──ack received──→ delivered ──ack displayed──→ displayed ─┤
   │                          │                       │                                    └─→ expired        (terminal)
   │                          │                       │
   │                          │                       └─event(ack_give_up) → failed       (terminal)
   │                          │
   │                          └─10 次仍无 received ack──→ failed   (terminal,reason=received_ack_timeout)
   │
   └─event(parse_reject|store_full)──────────────────────→ failed   (terminal)
```

**状态推进规则**(服务器):
- `queued → sent`:首次 publish 成功
- `sent → delivered`:收到 `received` ack
- `delivered → displayed`:收到 `displayed` ack
- `displayed → acknowledged`:收到 `acknowledged` ack
- `displayed → expired`:收到 `expired` ack
- `(queued|sent) → failed`:重试 10 次仍无 received,或收到 event(parse_reject/store_full/ack_give_up)

**重试停止条件**:`status NOT IN ('queued','sent')` 任一成立。

---

## 5. 重试策略

| 层 | 谁 | 触发 | 算法 | 上限 | 数据落点 |
|---|---|---|---|---|---|
| 下行 publish | 服务器 | `received` ack 未到 | 指数退避 base=2 s,max=300 s | 10 次 → `failed("received_ack_timeout")` | `messages.attempt_count / next_retry_at` |
| 上行 ack | 设备 | mqtt publish 失败或服务器未确认 | 指数退避 base=2 s,max=300 s | 10 次 → 上行 `event(ack_give_up)` 后从 NVS 删除 | NVS `tx_pending` blob |
| 设备重连 | 设备 | TCP/TLS/MQTT 断 | 指数退避 1→60 s | ∞ | — |

---

## 6. 设备 ACL 推导(服务器在 auth 阶段下发给 broker)

```
allow sub  device/{device_id}/cmd
allow sub  broadcast/all/cmd
allow sub  broadcast/dept/{dept}/cmd        (仅当 device.dept 非空)
allow sub  broadcast/role/{role}/cmd        (device.roles 中每个 role 一行)
allow pub  device/{device_id}/status
allow pub  device/{device_id}/uplink/ack
allow pub  device/{device_id}/uplink/event
allow pub  device/{device_id}/uplink/health
allow pub  device/{device_id}/uplink/profile
deny  其它
```

`dept` / `roles` 由控制台手工设置在 `devices` 表;变更后下次重连生效(可选:服务器主动 disconnect 该设备使其立即重连重新拿 ACL)。

---

## 7. 错误码(上行 event.reason 与服务器 last_error 共用词表)

| code | 含义 | 触发方 |
|---|---|---|
| `parse_reject:bad_json` | JSON 解析失败 | 设备 |
| `parse_reject:no_display` | title 与 body 都空 | 设备 |
| `parse_reject:bad_level` | level 字段非法 | 设备 |
| `store_full` | SPIFFS 消息槽满 | 设备 |
| `dedup_drop` | LRU 命中 | 设备 |
| `ack_give_up:received` | tx 重发 received ack 10 次失败 | 设备 |
| `ack_give_up:displayed` | 同上 displayed | 设备 |
| `ack_give_up:acknowledged` | 同上 acknowledged | 设备 |
| `ack_give_up:expired` | 同上 expired | 设备 |
| `received_ack_timeout` | 服务器下行重试 10 次未收到 received | 服务器 |
| `mqtt_publish_failed` | MQTT 发布失败(broker 未连) | 服务器 |
| `payload_json_invalid` | DB 中 payload_json 损坏 | 服务器 |
| `auth_batch_revoked` | 接入鉴权拒(批次已撤销) | 服务器 |
| `auth_hmac_mismatch` | HMAC 校验失败 | 服务器 |
| `auth_clock_skew` | 时间戳超出 ±300 s | 服务器 |
| `auth_nonce_replay` | nonce 已用过 | 服务器 |

---

## 8. 时间同步

- 设备启动后必须先 SNTP 同步成功才允许 MQTT CONNECT(HMAC 需要正确时间)。
- SNTP 服务器与 MQTT broker 建议同址(域名或 IP 一致),保证"网络可达"⇔"时间可同步"。
- 运行期 SNTP 每小时刷新一次。
- HMAC 容差 ±300 s,RTC 漂移在容差内(C3 ±1.7 s/天)。

---

## 9. 完整消息时序图

```
管理员/LLM   控制台      FastAPI      MQTT Broker     设备
   │           │            │             │            │
   ├──"通知"──▶│            │             │            │
   │           ├POST /msg ──▶ insert       │            │
   │           │            │ status=queued│            │
   │           │            ├─publish─────▶│            │
   │           │            │ status=sent  │ device/cmd │
   │           │            │              ├───────────▶│ parser → SPIFFS
   │           │            │              │            │ tx_emit_ack(received)
   │           │            │              │◀───────────┤
   │           │            │ ack=received │            │
   │           │            │ status=delivered          │
   │           │ SSE ◀──────┤              │            │
   │           │            │              │            │ msg_step_current
   │           │            │              │            │ tx_emit_ack(displayed)
   │           │            │              │◀───────────┤
   │           │            │ ack=displayed│            │
   │           │            │ status=displayed          │
   │           │ SSE ◀──────┤              │            │
   │           │            │              │            │ 用户摇一摇
   │           │            │              │            │ tx_emit_ack(acknowledged)
   │           │            │              │◀───────────┤
   │           │            │ ack=acknowledged          │
   │           │            │ status=acknowledged       │
   │           │ SSE ◀──────┤              │            │
```

---

## 10. 已知限制 / 待办

| 项 | 状态 | 备注 |
|---|---|---|
| `dedup_drop` event 上报 | 可选 | 设备 LRU 内部命中即可,不必上行;若上行只为可观测 |
| 多设备 ACL 即时生效 | 简化 | 现状靠下次重连;如需即时,需 broker 支持 ACL hot-reload + server 主动 disconnect |
| OTA | 不做 | 4 MB Flash 无双分区(论文已写) |
| 端侧日志远传 | 不做 | 串口排查 |

---

**文档结束**
