# BeaconOps 落地决策清单 v1

> 用途:记录"落地版"与"论文版"规格的所有偏差,便于答辩和回溯。
> 结论性偏差走 D 编号,后续如改回论文方案需新增条目说明。

---

## D-01:身份模型 — UUID 是批次许可,不是设备身份

| 项 | 论文(`01-通信协议规格.md` §1.2) | 落地(`protocol-v1.md` §1) |
|---|---|---|
| 设备身份来源 | 烧录 `DEVICE_UUID`(每台唯一) | `device_id = MAC 12 hex`(`esp_efuse_mac_get_default`,运行时取) |
| 烧录内容 | UUID + batch_id + batch_secret(每台一份固件) | batch_uuid + batch_secret(同批共享,**一份固件**) |
| 服务器白名单主键 | `devices.uuid` | `batches.batch_uuid` |
| 撤销粒度 | 单台 UUID revoke | 批次撤销 + 单台 disable(正交) |
| 工厂工艺 | 脚本生成 N 份固件 + 逐台烧 | 一份固件 + 逐台 USB 工厂工具写 NVS provisioning |

**理由**:
- MAC 全球唯一且 ESP32 自带,等于"免费 device_id";论文方案需要 N 份固件成本高
- "白名单 = 整批授权"更贴合现实场景(整批合作终止可一键下架,不需逐台维护)
- batch_secret 仍是同批共享(论文同),拆机风险等价

---

## D-02:删除 holder/user 概念

| 项 | 论文 | 落地 |
|---|---|---|
| 收件人模型 | `users` 表(name/dept/role/device_uuid) | **不存在**;`dept` 直接挂 `devices` |
| LLM 工具 | `query_user(name/dept/role) → user_id`,`send_message(target_user_id)` | `query_device(...)`,`send_message(device_id)` |
| 广播 scope | 通过 `users.dept / user_roles` 反查 device | 直接 `SELECT device_id FROM devices WHERE dept=?` |
| 控制台页面 | "持有人 / 设备 / 消息中心" 三页并行 | "设备 / 消息中心 / 批次白名单" |
| 操作员账号 | `admins` 表(独立) | 不变,仍独立 |

**理由**:
- 现状 users 表实际为空(远端确认),却被设为 LLM 单播必经路径,导致整个系统不可用
- 一台设备的"使用者"是个外部信息(可记 `devices.alias` 文本字段),不必建模
- 广播只保留部门维度,与"谁在用"解耦
- 对应用户原话:"首先不需要持有人这个概念"

---

## D-03:删除 TTS 与音频流(呼应 TODO "移除TTS")

| 项 | 论文 | 落地 |
|---|---|---|
| `audio` 字段 | MDL 必含,支持 `mode=tts_stream / url` | **删除** |
| `device/{id}/audio` topic | Opus 流式 / HTTP Range | **删除** |
| 端侧 audio_router / I²S 任务 | 必有 | 保留 I²S 硬件可用,但不挂 TTS;固件移除 audio_router 模块 |
| TTS 凭据下发 API | `GET /device/tts/credential` | **删除** |
| MDL 字段 | `display / audio / haptic / ack` | `title / body / level / ttl / ack` 单字段 |

**理由**:作者明确决定移除;音频不属于本期范畴。

---

## D-04:消息 9 状态机全暴露(前端不再聚合)

| 项 | 论文/现状 | 落地 |
|---|---|---|
| 状态枚举 | 多套混用(sent/delivered/displayed/acknowledged/expired/failed) | 9 状态:queued/sent/delivered/displayed/acknowledged/expired/failed + 中间事件 |
| 前端展示 | `groupStatus()` 折叠成 confirmed/unconfirmed/failed 三态 | **直接渲染原始状态** + attempt_count + next_retry_at + last_error + acks 时间轴 |
| 前端字段 | 没有电量/RSSI/IP/队列深度数值 | 设备卡片绑定 `health` 上报字段全部可见 |

**理由**:用户原话"前端都没有数值";信息从来没传到前端,不是前端 bug 而是协议没设计。

---

## D-05:协议字段精简(扁平化 + 单字段 ack)

| 项 | 论文 | 落地 |
|---|---|---|
| 下行字段 | `v / id / ts / ttl / level / source / display{} / audio{} / haptic{} / ack{}` | `id / ts / ttl / level / title / body / ack` |
| `level` 类型 | 字符串 | 字符串(禁用整数兼容) |
| `display` 嵌套 | `display.title / display.body` | 扁平 `title / body` |
| `ack` 字段 | 对象 `{required, method, timeout_ms, escalate_to}` | 字符串枚举 `none / received / displayed / acknowledged` |
| `source` | `{type, operator_id, operator_name}` | 移除(改在服务器端 `messages.source_type / operator` 列存) |
| 上行 ack | `{msg_id, device_id, type, ts, via}` | `{msg_id, kind, ts}` |

**理由**:嵌套与冗余字段在固件 parser 中产生大量兼容代码;source/via 在协议层无消费方,只该是服务器内部字段。

---

## D-06:新增 `uplink/event` 与 `uplink/health`

| topic | 论文 | 落地 |
|---|---|---|
| `device/{id}/uplink/event` | 不存在 | 新增,上报 `ack_give_up / parse_reject / store_full / dedup_drop` |
| `device/{id}/uplink/health` | 用 `telemetry` 包揽 | 拆分:health=低频运行态(电量/RSSI/IP/队列深度),profile=行为窗口 |
| `device/{id}/uplink/hb` | 论文未定义,固件实现了 | **删除**(被 health 替代) |
| `device/{id}/uplink/telemetry` | 论文定义 | **删除**(老 schema,被 profile 替代) |
| `device/{id}/shake` | 论文定义 | **删除**(摇晃只用作 ack 触发,不单独上行) |
| `system/time` / `system/notice` | 论文定义 | **暂不实现**(无消费方) |

**理由**:
- `uplink/event` 填补"端侧 ack 重发达上限后服务器永远不知"的黑洞(S9)
- 拆 health/profile 的目的:health 是装备状态(SLA / 维护),profile 是行为聚合(EC-BAID 算法源),完全不同的消费方与频率
- 删 hb/telemetry/shake 是清理协议遗物

---

## D-07:`level=warn|emergency` 强制 ack=acknowledged

| 项 | 论文 | 落地 |
|---|---|---|
| 是否要求摇一摇 | 由 `ack.required` 字段决定,LLM 可设 false | warn/emergency 服务器层强制 acknowledged,LLM 入参不允许覆盖 |

**理由**:防 prompt injection 让 LLM 把紧急消息设为 fire-and-forget。

---

## D-08:删除 `system/time` 时间同步通道

| 项 | 论文 | 落地 |
|---|---|---|
| 时间同步 | SNTP 主 + MQTT `system/time` 备 | 仅 SNTP;失败则不允许 MQTT CONNECT(HMAC 需要时间) |

**理由**:`system/time` 备方案需要 broker retain + 设备启动期订阅,逻辑分支多;SNTP 失败本身就意味着网络不通,MQTT 也不会通,无意义。

---

## D-09:OTA 不做(沿用论文)

不变。4 MB Flash 无双分区,串口烧录维护。论文已列为已知简化项。

---

## D-09b:MQTTS 由 nginx stream 终止(不走 Mosquitto 原生 TLS)

| 项 | 论文 | 落地 |
|---|---|---|
| Broker TLS | Mosquitto 直接听 8883,自管 cert | **nginx `stream {}`** 听 8883,复用部署域名证书,`proxy_pass` 到 `127.0.0.1:1883` |
| Mosquitto 监听 | 0.0.0.0 | `127.0.0.1` 单接口,不对公网暴露 |
| 证书续签 | 独立申请 | 复用 `cocandy.conf` 的 80 端口 acme-challenge,certbot 后挂 nginx 重载即可 |
| 配置位置 | — | `Server/services/beacon/scripts/nginx.stream.conf`;由 `/etc/nginx/nginx.conf` 顶层 glob `include /opt/server/services/*/scripts/nginx.stream.conf;` |
| 设备端 `BROKER_URI` | — | `mqtts://YOUR_BROKER_HOST:8883` |

**理由**:证书管理集中在 nginx;Mosquitto 不需重启即可轮换证书;后续如加 mTLS 也只在 nginx 侧配置,Mosquitto 维持最简形态。

---

## D-10:数据模型简化(配套 D-01~D-04)

| 表 | 论文 | 落地 |
|---|---|---|
| `users / user_roles / admin_roles` | 有 | **删** |
| `holders` 相关 view/路由 | 有 | **删** |
| `batches` | 无 | **新增**(`batch_uuid PK / alias / batch_secret / produced_at / produced_count / revoked / notes`) |
| `devices` | UUID PK + alias + user_id + ... | PK 改 `device_id`(MAC),新增 `batch_uuid FK / dept / enabled`;移除 `user_id` |
| `messages` | 主键 device_uuid | 主键 device_id |
| `acks` | `{msg_id, device_uuid, ack_type, via}` | `{msg_id, device_id, kind, ts}`(删 via) |
| `device_health_history`(可选) | 无 | 新增,时序,周期 30 s 一行;**只在控制台开启时启用** |
| `telemetry` | 有 | **删**(被 behavior 替代) |
| `behavior / behavior_decision / profile_7d` | 有 | 不变 |
| `audit_log / scheduled_tasks / confirm_tokens` | 有 | 不变 |
| `roles`(MCP guard) | 有 | 不变(操作员角色,与设备分组无关) |

> `admins` 表(操作员账户)与 `roles`(MCP guard 权限格)保留;它们与设备分组无关。

---

## D-11:实施次序(优先级)

1. ✅ 协议文档落地(本文 + `protocol-v1.md`)
2. ✅ 数据模型重写(`data-model-v1.md`,已在 `database.py` `SCHEMA` 实现,删表重建)
3. ✅ 固件改:运行时 device_id + 新 topic + event/health 上行 + 移除 audio/hb/telemetry/shake(5 个 bug 已修复,代码在 `\\wsl.localhost\Ubuntu-22.04\esp-item\BeaconOps\`)
4. ✅ 服务器后端重写(FastAPI 服务在 `Server/services/beacon/api/` 已上线)
5. ⏳ 前端重写(以设备为中心,见 `frontend-v1.md`)
6. ⏳ 切流量、删旧代码

---

**文档结束**
