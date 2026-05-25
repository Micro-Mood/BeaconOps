"""设备接入鉴权 — HMAC 校验 + ACL 推导。

固件 CONNECT 时:
  client_id = device_id (12 hex MAC)
  username  = batch_uuid
  password  = "<ts>:<nonce>:<hmac_hex>"
            其中 hmac_hex = HMAC_SHA256(batch_secret,
                                        device_id || "|" || ts || "|" || nonce)
"""
from __future__ import annotations
import hashlib
import hmac
import logging
import time
import re

from . import config
from . import database as db

log = logging.getLogger("beacon.identity")

_MAC_RE = re.compile(r"^[0-9a-f]{12}$")


def _now() -> int:
    return int(time.time())


def _hex_hmac(secret: str, message: str) -> str:
    return hmac.new(secret.encode("utf-8"), message.encode("utf-8"), hashlib.sha256).hexdigest()


async def verify_connect(client_id: str, username: str, password: str) -> tuple[bool, str]:
    """校验设备 CONNECT。返回 (ok, reason)。reason 失败时给可读原因。"""
    device_id = (client_id or "").strip().lower()
    batch_uuid = (username or "").strip()
    if not _MAC_RE.match(device_id):
        return False, "auth_bad_client_id"
    if not batch_uuid:
        return False, "auth_no_batch_uuid"

    parts = (password or "").split(":")
    if len(parts) != 3:
        return False, "auth_bad_password_format"
    try:
        ts = int(parts[0])
    except ValueError:
        return False, "auth_bad_ts"
    nonce, mac_hex = parts[1], parts[2].lower()
    if not nonce or len(mac_hex) != 64:
        return False, "auth_bad_hmac"

    now = _now()
    if abs(now - ts) > config.HMAC_MAX_SKEW_S:
        return False, "auth_clock_skew"

    batch = await db.fetchone(
        "SELECT batch_secret, revoked, produced_count FROM batches WHERE batch_uuid=?", (batch_uuid,)
    )
    if not batch:
        return False, "auth_batch_unknown"
    if batch["revoked"]:
        return False, "auth_batch_revoked"

    # 设备级禁用(若已注册过)
    existing = await db.fetchone(
        "SELECT enabled, batch_uuid FROM devices WHERE device_id=?", (device_id,)
    )
    if existing and not existing["enabled"]:
        return False, "auth_device_disabled"

    if not existing:
        batch_device_count = await db.fetchone(
            "SELECT COUNT(*) AS total FROM devices WHERE batch_uuid=?", (batch_uuid,)
        )
        if (batch_device_count["total"] or 0) >= batch["produced_count"]:
            return False, "auth_batch_capacity_reached"

    expected = _hex_hmac(batch["batch_secret"], f"{device_id}|{ts}|{nonce}")
    if not hmac.compare_digest(expected, mac_hex):
        return False, "auth_hmac_mismatch"

    # nonce 重放检查
    seen = await db.fetchone("SELECT 1 FROM mqtt_nonces WHERE nonce=?", (nonce,))
    if seen:
        return False, "auth_nonce_replay"
    await db.execute(
        "INSERT INTO mqtt_nonces(nonce, device_id, seen_at, expires_at) VALUES(?,?,?,?)",
        (nonce, device_id, now, now + config.NONCE_TTL_S),
    )

    # 首次见 → upsert devices(保留已存在的 alias/dept)
    if not existing:
        await db.execute(
            "INSERT INTO devices(device_id, batch_uuid, first_seen_at, last_seen_at, "
            "                    created_at, updated_at) "
            "VALUES(?,?,?,?,?,?)",
            (device_id, batch_uuid, now, now, now, now),
        )
    else:
        if existing["batch_uuid"] != batch_uuid:
            log.info("[auth] repair device batch device=%s from=%s to=%s",
                     device_id, existing["batch_uuid"], batch_uuid)
        await db.execute(
            "UPDATE devices SET batch_uuid=?, last_seen_at=?, updated_at=? WHERE device_id=?",
            (batch_uuid, now, now, device_id),
        )

    return True, "ok"


async def build_acl(device_id: str) -> list[dict]:
    """返回 mosquitto-go-auth HTTP backend 期望的 ACL 列表。

    具体字段名取决于 broker 端配置;此处给出通用 {pattern, access} 形式,
    broker 端把 access(1=sub, 2=pub) 翻译。
    """
    dev = await db.fetchone(
        "SELECT d.dept, d.enabled, b.revoked FROM devices d "
        "JOIN batches b ON b.batch_uuid=d.batch_uuid WHERE d.device_id=?", (device_id,)
    )
    if not dev or not dev["enabled"] or dev["revoked"]:
        return []
    dept = (dev or {}).get("dept", "") or ""

    acl: list[dict] = [
        {"topic": f"device/{device_id}/cmd", "access": "sub"},
        {"topic": "broadcast/all/cmd",       "access": "sub"},
        {"topic": f"device/{device_id}/status",         "access": "pub"},
        {"topic": f"device/{device_id}/uplink/ack",     "access": "pub"},
        {"topic": f"device/{device_id}/uplink/event",   "access": "pub"},
        {"topic": f"device/{device_id}/uplink/health",  "access": "pub"},
        {"topic": f"device/{device_id}/uplink/profile", "access": "pub"},
    ]
    if dept:
        acl.append({"topic": f"broadcast/dept/{dept}/cmd", "access": "sub"})
    return acl
