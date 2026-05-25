"""消息核心 — recipient 级下行、重试、状态推进、SSE 推送。

目标语义:
  选中的设备都会写入 message_recipients；在线设备立即投递，离线设备保持
  queued，设备上线后补发。messages 表只保存整条消息的聚合状态。
"""
from __future__ import annotations
import asyncio
import json
import logging
import secrets
import time

from . import config
from . import database as db
from . import events
from . import mqtt_client

log = logging.getLogger("beacon.msg")

_VALID_LEVELS = ("info", "notice", "warn", "emergency")
_VALID_ACKS = ("none", "received", "displayed", "acknowledged")
_VALID_TARGET = ("device", "all", "dept")
_TERMINAL = ("acknowledged", "expired", "failed")


def select_columns(alias: str = "m") -> str:
    p = f"{alias}."
    return (
        f"{alias}.*, "
        f"COALESCE({p}acknowledged_at, {p}displayed_at, {p}delivered_at, "
        f"{p}failed_at, {p}expired_at, {p}sent_at, {p}created_at) AS updated_at, "
        f"(SELECT COUNT(*) FROM message_recipients r WHERE r.msg_id={p}msg_id) AS recipients_total, "
        f"(SELECT COUNT(*) FROM message_recipients r WHERE r.msg_id={p}msg_id AND r.status IN ('delivered','displayed','acknowledged')) AS recipients_delivered, "
        f"(SELECT COUNT(*) FROM message_recipients r WHERE r.msg_id={p}msg_id AND r.status IN ('displayed','acknowledged')) AS recipients_displayed, "
        f"(SELECT COUNT(*) FROM message_recipients r WHERE r.msg_id={p}msg_id AND r.status='acknowledged') AS recipients_acknowledged, "
        f"(SELECT COUNT(*) FROM message_recipients r WHERE r.msg_id={p}msg_id AND r.status='expired') AS recipients_expired, "
        f"(SELECT COUNT(*) FROM message_recipients r WHERE r.msg_id={p}msg_id AND r.status='failed') AS recipients_failed"
    )


def _now() -> int:
    return int(time.time())


def _backoff(attempt: int) -> int:
    shift = max(0, min(attempt - 1, 8))
    return min(config.DOWNLINK_BACKOFF_BASE_S << shift, config.DOWNLINK_BACKOFF_MAX_S)


def _build_payload(msg: dict) -> dict:
    return {
        "id":    msg["msg_id"],
        "ts":    msg["sent_at"] or _now(),
        "ttl":   msg["ttl"],
        "level": msg["level"],
        "title": msg["title"],
        "body":  msg["body"],
        "ack":   msg["ack_mode"],
    }


def _is_message_expired(msg: dict, now: int) -> bool:
    ttl = int(msg.get("ttl") or 0)
    return ttl > 0 and int(msg.get("created_at") or 0) + ttl <= now


async def create_message(
    *,
    target_kind: str,
    target_value: str,
    level: str,
    title: str,
    body: str,
    ttl: int,
    ack_mode: str,
    source_type: str,
    operator: str,
) -> dict:
    if target_kind not in _VALID_TARGET:
        raise ValueError(f"bad target_kind: {target_kind}")
    if level not in _VALID_LEVELS:
        raise ValueError(f"bad level: {level}")
    if ack_mode not in _VALID_ACKS:
        raise ValueError(f"bad ack_mode: {ack_mode}")
    if not (title or body):
        raise ValueError("title and body cannot both be empty")
    if level in ("warn", "emergency"):
        ack_mode = "acknowledged"
    if level == "emergency":
        ttl = 0

    msg_id = secrets.token_hex(16)
    now = _now()
    recipients = await _resolve_recipients(target_kind, target_value)
    if target_kind == "device" and not recipients:
        raise ValueError(f"unknown, disabled, or revoked device: {target_value}")
    if target_kind in ("all", "dept") and not recipients:
        raise ValueError("no active recipients")

    payload_preview = {
        "id": msg_id, "ts": now, "ttl": ttl, "level": level,
        "title": title, "body": body, "ack": ack_mode,
    }

    async with db.begin() as c:
        await c.execute(
            "INSERT INTO messages(msg_id, target_kind, target_value, level, title, body, ttl, "
            "                     ack_mode, source_type, operator, status, attempt_count, "
            "                     created_at, payload_json) "
            "VALUES(?,?,?,?,?,?,?,?,?,?, 'queued', 0, ?, ?)",
            (msg_id, target_kind, target_value, level, title, body, ttl, ack_mode,
             source_type, operator, now, json.dumps(payload_preview, ensure_ascii=False)),
        )
        for device_id in recipients:
            await c.execute(
                "INSERT INTO message_recipients(msg_id, device_id, status) VALUES(?,?, 'queued')",
                (msg_id, device_id),
            )

    events.emit(
        "message.created",
        msg_id=msg_id,
        target_kind=target_kind,
        target_value=target_value,
        level=level,
        ack_mode=ack_mode,
        ttl=ttl,
        status="queued",
        recipients_count=len(recipients),
        created_at=now,
        source_type=source_type,
        operator=operator,
    )
    asyncio.create_task(_publish_due_for_message(msg_id, force=True))
    return {"msg_id": msg_id, "recipients_count": len(recipients), "ack_mode": ack_mode, "ttl": ttl}


async def _resolve_recipients(kind: str, value: str) -> list[str]:
    base = (
        "SELECT d.device_id FROM devices d "
        "JOIN batches b ON b.batch_uuid=d.batch_uuid "
        "WHERE d.enabled=1 AND b.revoked=0"
    )
    if kind == "device":
        row = await db.fetchone(base + " AND d.device_id=?", (value,))
        return [row["device_id"]] if row else []
    if kind == "all":
        rows = await db.fetchall(base + " ORDER BY d.device_id")
    elif kind == "dept":
        rows = await db.fetchall(base + " AND d.dept=? ORDER BY d.device_id", (value,))
    else:
        rows = []
    return [r["device_id"] for r in rows]


async def _publish_due_for_message(
    msg_id: str,
    *,
    device_id: str = "",
    force: bool = False,
) -> None:
    msg = await db.fetchone("SELECT * FROM messages WHERE msg_id=?", (msg_id,))
    if not msg or msg["status"] in _TERMINAL:
        return

    now = _now()
    if _is_message_expired(msg, now):
        await _expire_recipients(msg_id, now, device_id=device_id)
        await _refresh_message_aggregate(msg_id, now)
        return

    rows = await _due_recipients(msg_id, now, device_id=device_id, force=force)
    if not rows:
        await _refresh_message_aggregate(msg_id, now)
        return

    payload = _build_payload(msg)
    payload_json = json.dumps(payload, ensure_ascii=False)
    await db.execute("UPDATE messages SET payload_json=? WHERE msg_id=?", (payload_json, msg_id))

    for rec in rows:
        await _publish_to_recipient(msg, rec, payload)

    await _refresh_message_aggregate(msg_id, _now())


async def _due_recipients(msg_id: str, now: int, *, device_id: str = "", force: bool = False) -> list[dict]:
    where = [
        "r.msg_id=?",
        "r.status IN ('queued','sent')",
        "d.enabled=1",
        "d.online=1",
        "b.revoked=0",
    ]
    params: list = [msg_id]
    if device_id:
        where.append("r.device_id=?")
        params.append(device_id)
    if not force:
        where.append("(r.status='queued' OR r.next_retry_at IS NULL OR r.next_retry_at<=?)")
        params.append(now)
    return await db.fetchall(
        "SELECT r.msg_id, r.device_id, r.status, r.attempt_count, r.next_retry_at "
        "FROM message_recipients r "
        "JOIN devices d ON d.device_id=r.device_id "
        "JOIN batches b ON b.batch_uuid=d.batch_uuid "
        f"WHERE {' AND '.join(where)} ORDER BY r.device_id LIMIT 64",
        tuple(params),
    )


async def _publish_to_recipient(msg: dict, rec: dict, payload: dict) -> None:
    msg_id = msg["msg_id"]
    device_id = rec["device_id"]
    now = _now()
    attempt = int(rec.get("attempt_count") or 0) + 1
    if attempt > config.DOWNLINK_MAX_ATTEMPTS:
        await _mark_recipient_failed(msg_id, device_id, "received_ack_timeout", now=now, refresh=False)
        return

    topic = f"device/{device_id}/cmd"
    ok = await mqtt_client.publish(topic, payload, qos=1, retain=False)
    if ok:
        if msg["ack_mode"] == "none":
            await db.execute(
                "UPDATE message_recipients SET status='delivered', attempt_count=?, "
                "       last_send_at=?, next_retry_at=NULL, delivered_at=COALESCE(delivered_at, ?), "
                "       failed_at=NULL, last_error='' "
                "WHERE msg_id=? AND device_id=? AND status IN ('queued','sent')",
                (attempt, now, now, msg_id, device_id),
            )
        else:
            await db.execute(
                "UPDATE message_recipients SET status='sent', attempt_count=?, "
                "       last_send_at=?, next_retry_at=?, failed_at=NULL, last_error='' "
                "WHERE msg_id=? AND device_id=? AND status IN ('queued','sent')",
                (attempt, now, now + _backoff(attempt), msg_id, device_id),
            )
        await db.execute(
            "INSERT OR IGNORE INTO message_events(msg_id, device_id, kind, detail, ts, recorded_at) "
            "VALUES(?,?,?,?,?,?)",
            (msg_id, device_id, f"publish:{attempt}", topic, now, now),
        )
        events.emit("message.recipient_update", msg_id=msg_id, device_id=device_id,
                    status=("delivered" if msg["ack_mode"] == "none" else "sent"), ts=now)
        log.info("[downlink] %s -> %s attempt=%s", msg_id, device_id, attempt)
        return

    if attempt >= config.DOWNLINK_MAX_ATTEMPTS:
        await _mark_recipient_failed(msg_id, device_id, "mqtt_publish_failed", now=now, refresh=False)
        return
    await db.execute(
        "UPDATE message_recipients SET attempt_count=?, next_retry_at=?, last_error=? "
        "WHERE msg_id=? AND device_id=? AND status IN ('queued','sent')",
        (attempt, now + _backoff(attempt), "mqtt_publish_failed", msg_id, device_id),
    )
    log.warning("[downlink] publish failed %s -> %s attempt=%s", msg_id, device_id, attempt)


async def retry_loop() -> None:
    while True:
        try:
            await asyncio.sleep(config.DOWNLINK_RETRY_TICK_S)
            await _tick()
        except asyncio.CancelledError:
            return
        except Exception:
            log.exception("retry_loop")


async def _tick() -> None:
    now = _now()
    await _expire_overdue(now)
    rows = await db.fetchall(
        "SELECT DISTINCT r.msg_id FROM message_recipients r "
        "JOIN messages m ON m.msg_id=r.msg_id "
        "JOIN devices d ON d.device_id=r.device_id "
        "JOIN batches b ON b.batch_uuid=d.batch_uuid "
        "WHERE r.status IN ('queued','sent') "
        "  AND m.status NOT IN ('acknowledged','expired','failed') "
        "  AND d.enabled=1 AND d.online=1 AND b.revoked=0 "
        "  AND (r.status='queued' OR r.next_retry_at IS NULL OR r.next_retry_at<=?) "
        "ORDER BY r.msg_id LIMIT 32",
        (now,),
    )
    for row in rows:
        await _publish_due_for_message(row["msg_id"])


async def on_device_online(device_id: str) -> None:
    now = _now()
    await _expire_overdue(now, device_id=device_id)
    rows = await db.fetchall(
        "SELECT DISTINCT r.msg_id FROM message_recipients r "
        "JOIN messages m ON m.msg_id=r.msg_id "
        "WHERE r.device_id=? AND r.status IN ('queued','sent') "
        "  AND m.status NOT IN ('acknowledged','expired','failed') "
        "ORDER BY m.created_at ASC LIMIT 32",
        (device_id,),
    )
    for row in rows:
        await _publish_due_for_message(row["msg_id"], device_id=device_id, force=True)


async def _expire_overdue(now: int, *, device_id: str = "") -> None:
    params: list = [now]
    device_filter = ""
    if device_id:
        device_filter = " AND EXISTS (SELECT 1 FROM message_recipients r WHERE r.msg_id=m.msg_id AND r.device_id=?)"
        params.append(device_id)
    rows = await db.fetchall(
        "SELECT m.msg_id FROM messages m "
        "WHERE m.status NOT IN ('acknowledged','expired','failed') "
        "  AND m.ttl>0 AND m.created_at + m.ttl <= ?" + device_filter,
        tuple(params),
    )
    for row in rows:
        await _expire_recipients(row["msg_id"], now, device_id=device_id)
        await _refresh_message_aggregate(row["msg_id"], now)


async def _expire_recipients(msg_id: str, now: int, *, device_id: str = "") -> None:
    params: list = [now, msg_id]
    device_filter = ""
    if device_id:
        device_filter = " AND device_id=?"
        params.append(device_id)
    await db.execute(
        "UPDATE message_recipients SET status='expired', expired_at=COALESCE(expired_at, ?), "
        "       next_retry_at=NULL, last_error='' "
        "WHERE msg_id=? AND status NOT IN ('acknowledged','expired','failed')" + device_filter,
        tuple(params),
    )


async def _mark_failed(msg_id: str, reason: str) -> None:
    now = _now()
    msg = await db.fetchone("SELECT status FROM messages WHERE msg_id=?", (msg_id,))
    if not msg or msg["status"] in _TERMINAL:
        return
    await db.execute(
        "UPDATE message_recipients SET status='failed', failed_at=?, next_retry_at=NULL, last_error=? "
        "WHERE msg_id=? AND status NOT IN ('acknowledged','expired','failed')",
        (now, reason, msg_id),
    )
    await _refresh_message_aggregate(msg_id, now)


async def _mark_expired(msg_id: str) -> None:
    now = _now()
    await _expire_recipients(msg_id, now)
    await _refresh_message_aggregate(msg_id, now)


async def _mark_recipient_failed(
    msg_id: str,
    device_id: str,
    reason: str,
    *,
    now: int | None = None,
    refresh: bool = True,
) -> None:
    now = now or _now()
    await db.execute(
        "UPDATE message_recipients SET status='failed', failed_at=?, next_retry_at=NULL, last_error=? "
        "WHERE msg_id=? AND device_id=? AND status NOT IN ('acknowledged','expired','failed')",
        (now, reason, msg_id, device_id),
    )
    events.emit("message.recipient_update", msg_id=msg_id, device_id=device_id,
                status="failed", failed_at=now, reason=reason)
    if refresh:
        await _refresh_message_aggregate(msg_id, now)


_ACK_KIND_TO_STATUS = {
    "received":     "delivered",
    "displayed":    "displayed",
    "acknowledged": "acknowledged",
    "expired":      "expired",
}


async def on_ack(device_id: str, msg_id: str, kind: str, ts: int) -> None:
    if kind not in _ACK_KIND_TO_STATUS:
        return
    now = _now()
    msg = await db.fetchone("SELECT status FROM messages WHERE msg_id=?", (msg_id,))
    if not msg:
        log.warning("[ack] unknown msg_id=%s from device=%s kind=%s", msg_id, device_id, kind)
        return
    try:
        await db.execute(
            "INSERT INTO message_events(msg_id, device_id, kind, ts, recorded_at) VALUES(?,?,?,?,?)",
            (msg_id, device_id, kind, ts, now),
        )
    except Exception:
        pass

    new_status = _ACK_KIND_TO_STATUS[kind]
    ts_col = {
        "delivered": "delivered_at",
        "displayed": "displayed_at",
        "acknowledged": "acknowledged_at",
        "expired": "expired_at",
    }[new_status]
    rec = await db.fetchone(
        "SELECT status FROM message_recipients WHERE msg_id=? AND device_id=?",
        (msg_id, device_id),
    )
    prev_rec = (rec or {}).get("status", "queued")
    if not rec:
        await db.execute(
            f"INSERT OR IGNORE INTO message_recipients(msg_id, device_id, status, {ts_col}) "
            "VALUES(?,?,?,?)",
            (msg_id, device_id, new_status, ts),
        )
    elif (prev_rec == "failed" or not _is_terminal(prev_rec)) and _progress_rank(new_status) > _progress_rank(prev_rec):
        await db.execute(
            f"UPDATE message_recipients SET status=?, {ts_col}=?, next_retry_at=NULL, "
            "failed_at=NULL, last_error='' WHERE msg_id=? AND device_id=?",
            (new_status, ts, msg_id, device_id),
        )
    events.emit("message.recipient_update", msg_id=msg_id, device_id=device_id,
                status=new_status, **{ts_col: ts})

    await _refresh_message_aggregate(msg_id, ts)
    events.emit("message.event", msg_id=msg_id, device_id=device_id, kind=kind, ts=ts)


async def _refresh_message_aggregate(msg_id: str, ts: int) -> None:
    msg = await db.fetchone("SELECT status, ack_mode FROM messages WHERE msg_id=?", (msg_id,))
    if not msg:
        return
    rows = await db.fetchall(
        "SELECT status, attempt_count, next_retry_at, last_send_at, last_error FROM message_recipients "
        "WHERE msg_id=?",
        (msg_id,),
    )
    if not rows:
        return

    statuses = [r["status"] for r in rows]
    active_statuses = [s for s in statuses if s not in ("expired", "failed")]
    target_status = {
        "none": "delivered",
        "received": "delivered",
        "displayed": "displayed",
        "acknowledged": "acknowledged",
    }.get(msg["ack_mode"], "acknowledged")
    target_rank = _progress_rank(target_status)
    pending_active = [s for s in active_statuses if _progress_rank(s) < target_rank]
    if active_statuses and pending_active:
        def all_at_least(status: str) -> bool:
            need = _progress_rank(status)
            return all(_progress_rank(s) >= need for s in active_statuses)

        if all_at_least("acknowledged"):
            new_status = "acknowledged"
        elif all_at_least("displayed"):
            new_status = "displayed"
        elif all_at_least("delivered"):
            new_status = "delivered"
        elif all_at_least("sent"):
            new_status = "sent"
        else:
            new_status = "queued"
    else:
        if any(s == "failed" for s in statuses):
            new_status = "failed"
        elif any(s == "expired" for s in statuses):
            new_status = "expired"
        elif all(_progress_rank(s) >= _progress_rank("acknowledged") for s in active_statuses):
            new_status = "acknowledged"
        elif all(_progress_rank(s) >= _progress_rank("displayed") for s in active_statuses):
            new_status = "displayed"
        else:
            new_status = "delivered"

    max_attempt = max(int(r.get("attempt_count") or 0) for r in rows)
    last_send = max((int(r["last_send_at"] or 0) for r in rows), default=0) or None
    retry_values = [int(r["next_retry_at"]) for r in rows if r.get("next_retry_at")]
    next_retry = min(retry_values) if retry_values else None
    last_error = ""
    for r in rows:
        if r.get("last_error"):
            last_error = r["last_error"]
            break

    ts_col = {
        "sent": "sent_at",
        "delivered": "delivered_at",
        "displayed": "displayed_at",
        "acknowledged": "acknowledged_at",
        "expired": "expired_at",
        "failed": "failed_at",
    }.get(new_status)
    if ts_col:
        await db.execute(
            f"UPDATE messages SET status=?, attempt_count=?, last_send_at=?, next_retry_at=?, "
            f"{ts_col}=COALESCE({ts_col}, ?), last_error=? WHERE msg_id=?",
            (new_status, max_attempt, last_send, next_retry, ts, last_error, msg_id),
        )
    else:
        await db.execute(
            "UPDATE messages SET status=?, attempt_count=?, last_send_at=?, next_retry_at=?, last_error=? "
            "WHERE msg_id=?",
            (new_status, max_attempt, last_send, next_retry, last_error, msg_id),
        )
    if msg["status"] != new_status:
        events.emit("message.status_change", msg_id=msg_id,
                    **{"from": msg["status"], "to": new_status, "ts": ts})


_STATUS_RANK = {
    "queued": 0, "sent": 1, "delivered": 2, "displayed": 3,
    "acknowledged": 4, "expired": 4, "failed": 4,
}

_PROGRESS_RANK = {
    "queued": 0, "failed": 0, "expired": 0, "sent": 1,
    "delivered": 2, "displayed": 3, "acknowledged": 4,
}


def _rank(s: str) -> int:
    return _STATUS_RANK.get(s, -1)


def _progress_rank(s: str) -> int:
    return _PROGRESS_RANK.get(s, -1)


def _is_terminal(s: str) -> bool:
    return s in _TERMINAL


async def on_device_event(device_id: str, kind: str, msg_id: str, detail: str, ts: int) -> None:
    """处理 uplink/event:ack_give_up / parse_reject / store_full / dedup_drop。"""
    now = _now()
    try:
        await db.execute(
            "INSERT INTO message_events(msg_id, device_id, kind, detail, ts, recorded_at) "
            "VALUES(?,?,?,?,?,?)",
            (msg_id or "", device_id, kind, detail, ts, now),
        )
    except Exception:
        pass

    if kind in ("ack_give_up", "parse_reject", "store_full"):
        reason = f"device_{kind}:{detail}" if detail else f"device_{kind}"
        if msg_id:
            await _mark_recipient_failed(msg_id, device_id, reason)
        events.emit("message.event", msg_id=msg_id, device_id=device_id, kind=kind, detail=detail, ts=ts)
    elif kind == "dedup_drop":
        events.emit("message.event", msg_id=msg_id, device_id=device_id, kind=kind, ts=ts)


async def cancel(msg_id: str) -> bool:
    msg = await db.fetchone("SELECT status FROM messages WHERE msg_id=?", (msg_id,))
    if not msg or msg["status"] not in ("queued", "sent"):
        return False
    await _mark_failed(msg_id, "cancelled_by_operator")
    return True
