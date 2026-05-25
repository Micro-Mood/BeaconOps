"""MQTT uplink → 业务路由。"""
from __future__ import annotations
import logging
import re
import time

from . import database as db
from . import events
from . import messages as msg_svc

log = logging.getLogger("beacon.uplink")

_TOPIC_RE = re.compile(r"^device/([0-9a-f]{12})/(status|uplink/(ack|event|health|profile))$")


def _now() -> int:
    return int(time.time())


def _int(payload: dict, key: str, default: int = 0) -> int:
    try:
        return int(payload.get(key, default))
    except (TypeError, ValueError):
        return default


async def _accept_uplink(device_id: str, payload: dict | None = None) -> bool:
    row = await db.fetchone(
        "SELECT d.enabled, d.online, b.revoked, b.batch_uuid FROM devices d "
        "JOIN batches b ON b.batch_uuid=d.batch_uuid WHERE d.device_id=?",
        (device_id,),
    )
    if not row:
        log.warning("drop uplink from unauthenticated device=%s", device_id)
        return False
    if row["enabled"] and not row["revoked"]:
        return True
    if row["online"]:
        await db.execute("UPDATE devices SET online=0, updated_at=? WHERE device_id=?", (_now(), device_id))
    log.warning("drop uplink from disabled/revoked device=%s", device_id)
    return False


async def dispatch(topic: str, payload: dict) -> None:
    m = _TOPIC_RE.match(topic)
    if not m:
        return
    device_id, sub = m.group(1), m.group(2)
    if sub == "status":
        await _on_status(device_id, payload)
    elif sub == "uplink/ack":
        await _on_ack(device_id, payload)
    elif sub == "uplink/event":
        await _on_event(device_id, payload)
    elif sub == "uplink/health":
        await _on_health(device_id, payload)
    elif sub == "uplink/profile":
        await _on_profile(device_id, payload)


async def _on_status(device_id: str, payload: dict) -> None:
    if not await _accept_uplink(device_id, payload):
        return
    online = 1 if payload.get("online") else 0
    fw = str(payload.get("fw", ""))
    now = _now()
    await db.execute(
        "UPDATE devices SET online=?, fw_version=COALESCE(NULLIF(?, ''), fw_version), "
        "                   last_seen_at=?, updated_at=? WHERE device_id=?",
        (online, fw, now, now, device_id),
    )
    device = await db.fetchone(
        "SELECT device_id, batch_uuid, alias, dept, enabled, online, updated_at FROM devices WHERE device_id=?",
        (device_id,),
    )
    events.emit("device.online" if online else "device.offline",
                device_id=device_id, fw=fw, ts=now,
                batch_uuid=(device or {}).get("batch_uuid", ""),
                alias=(device or {}).get("alias", ""),
                dept=(device or {}).get("dept", ""),
                enabled=(device or {}).get("enabled", 1),
                online=(device or {}).get("online", online),
                updated_at=(device or {}).get("updated_at", now))
    if online:
        await msg_svc.on_device_online(device_id)


async def _on_ack(device_id: str, payload: dict) -> None:
    if not await _accept_uplink(device_id, payload):
        return
    msg_id = str(payload.get("msg_id", ""))
    kind   = str(payload.get("kind", ""))
    ts     = _int(payload, "ts", _now())
    if not msg_id or not kind:
        return
    await msg_svc.on_ack(device_id, msg_id, kind, ts)


async def _on_event(device_id: str, payload: dict) -> None:
    if not await _accept_uplink(device_id, payload):
        return
    kind   = str(payload.get("kind", ""))
    msg_id = str(payload.get("msg_id", ""))
    reason = str(payload.get("reason", ""))
    ts     = _int(payload, "ts", _now())
    if not kind:
        return
    await msg_svc.on_device_event(device_id, kind, msg_id, reason, ts)


async def _on_health(device_id: str, payload: dict) -> None:
    if not await _accept_uplink(device_id, payload):
        return
    now = _now()
    fields = {
        "battery_soc":    _int(payload, "battery", -1),
        "charging":       1 if payload.get("charging") else 0,
        "rssi":           _int(payload, "rssi", 0),
        "last_ip":        str(payload.get("ip", "")),
        "fw_version":     str(payload.get("fw", "")) or None,
        "uptime_s":       _int(payload, "uptime_s", 0),
        "spiffs_pending": _int(payload, "spiffs_pending", 0),
        "ack_pending":    _int(payload, "ack_pending", 0),
        "drop_count":     _int(payload, "drop_count", 0),
        "last_seen_at":   now,
        "last_health_at": now,
        "updated_at":     now,
    }
    # 只更新非空字段
    sets = []
    vals: list = []
    for k, v in fields.items():
        if v is None:
            continue
        if k == "fw_version":
            sets.append("fw_version=COALESCE(NULLIF(?, ''), fw_version)")
            vals.append(v)
        else:
            sets.append(f"{k}=?")
            vals.append(v)
    vals.append(device_id)
    await db.execute(f"UPDATE devices SET {', '.join(sets)} WHERE device_id=?", tuple(vals))

    from . import config as cfg
    if cfg.HEALTH_HISTORY_ENABLED:
        await db.execute(
            "INSERT INTO device_health_history(device_id, ts, battery_soc, charging, rssi, "
            "                                  spiffs_pending, ack_pending) "
            "VALUES(?,?,?,?,?,?,?)",
            (device_id, _int(payload, "ts", now),
             fields["battery_soc"], fields["charging"], fields["rssi"],
             fields["spiffs_pending"], fields["ack_pending"]),
        )

    events.emit("device.health", device_id=device_id,
                battery_soc=fields["battery_soc"], rssi=fields["rssi"],
                ack_pending=fields["ack_pending"], spiffs_pending=fields["spiffs_pending"],
                ts=_int(payload, "ts", now),
                batch_uuid=((await db.fetchone("SELECT batch_uuid FROM devices WHERE device_id=?", (device_id,))) or {}).get("batch_uuid", ""))


async def _on_profile(device_id: str, payload: dict) -> None:
    if not await _accept_uplink(device_id, payload):
        return
    ts = _int(payload, "ts", _now())
    inserted = False
    try:
        await db.execute(
            "INSERT INTO behavior(device_id, ts, win_s, static_s, walk_slow_s, walk_fast_s, "
            "                     run_s, shake_or_fall_s, shake_n, intensity_avg, steps, received_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
            (device_id, ts,
             int(payload.get("win", 60)),
             int(payload.get("static", 0)),
             int(payload.get("walk_slow", 0)),
             int(payload.get("walk_fast", 0)),
             int(payload.get("run", 0)),
             int(payload.get("shake_or_fall", 0)),
             int(payload.get("shake_n", 0)),
             float(payload.get("intensity_avg", 0)),
             (int(payload["steps"]) if payload.get("steps") is not None else None),
             _now()),
        )
        inserted = True
    except Exception as e:
        # 唯一索引重复时忽略；其他错误记录以便排查
        if "UNIQUE" not in str(e).upper():
            log.warning("[uplink] profile insert error device=%s ts=%d: %s", device_id, ts, e)
    await db.execute(
        "UPDATE devices SET last_seen_at=?, updated_at=? WHERE device_id=?",
        (_now(), _now(), device_id),
    )
    if inserted:
        events.emit("device.behavior", device_id=device_id, ts=ts)
