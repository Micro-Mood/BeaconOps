"""批次管理。"""
from __future__ import annotations
import asyncio
import secrets
import time

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field

from .. import audit
from .. import auth as A
from .. import broker_control
from .. import database as db
from .. import events

router = APIRouter()


def _now() -> int:
    return int(time.time())


async def _delete_batch_devices(conn, device_ids: list[str]) -> None:
    if not device_ids:
        return
    placeholders = ",".join("?" for _ in device_ids)
    params = tuple(device_ids)
    await conn.execute(f"DELETE FROM message_recipients WHERE device_id IN ({placeholders})", params)
    await conn.execute(f"DELETE FROM message_events WHERE device_id IN ({placeholders})", params)
    await conn.execute(f"DELETE FROM device_health_history WHERE device_id IN ({placeholders})", params)
    await conn.execute(f"DELETE FROM mqtt_nonces WHERE device_id IN ({placeholders})", params)
    await conn.execute(f"DELETE FROM behavior WHERE device_id IN ({placeholders})", params)


class BatchCreateIn(BaseModel):
    batch_uuid: str = Field(min_length=1)
    alias: str = ""
    produced_count: int = Field(default=0, ge=0)
    notes: str = ""


@router.get("")
async def list_batches(admin: dict = Depends(A.current_admin)):
    rows = await db.fetchall(
        "SELECT b.batch_uuid, b.alias, b.produced_at, b.produced_count, b.revoked, "
        "       b.revoked_at, b.revoked_reason, b.notes, b.created_at, b.updated_at, "
        "       (SELECT COUNT(*) FROM devices d WHERE d.batch_uuid=b.batch_uuid) AS device_count, "
        "       (SELECT COUNT(*) FROM devices d WHERE d.batch_uuid=b.batch_uuid AND d.online=1) AS online_count "
        "FROM batches b WHERE b.deleted_at IS NULL ORDER BY b.created_at DESC, b.batch_uuid DESC"
    )
    return {"batches": rows}


@router.post("")
async def create_batch(body: BatchCreateIn, admin: dict = Depends(A.require_role("admin", "operator"))):
    if not body.batch_uuid.strip():
        raise HTTPException(400, "batch_uuid required")
    existing = await db.fetchone("SELECT 1 FROM batches WHERE batch_uuid=?", (body.batch_uuid,))
    if existing:
        raise HTTPException(409, "batch_uuid exists")
    secret = secrets.token_hex(32)
    now = _now()
    await db.execute(
        "INSERT INTO batches(batch_uuid, alias, batch_secret, produced_at, produced_count, "
        "                    notes, created_at, updated_at) VALUES(?,?,?,?,?,?,?,?)",
        (body.batch_uuid, body.alias, secret, now, body.produced_count, body.notes, now, now),
    )
    events.emit(
        "batch.created",
        batch_uuid=body.batch_uuid,
        alias=body.alias,
        produced_count=body.produced_count,
        revoked=0,
        notes=body.notes,
        created_at=now,
        updated_at=now,
    )
    await audit.log(actor_type="admin", actor=admin["username"], action="batch_create",
                    target_kind="batch", target_value=body.batch_uuid)
    return {"batch_uuid": body.batch_uuid, "batch_secret": secret}


@router.get("/{batch_uuid}")
async def get_batch(batch_uuid: str, admin: dict = Depends(A.current_admin)):
    row = await db.fetchone(
        "SELECT batch_uuid, alias, batch_secret, produced_at, produced_count, revoked, "
        "       revoked_at, revoked_reason, notes, created_at, updated_at "
        "FROM batches WHERE batch_uuid=? AND deleted_at IS NULL", (batch_uuid,),
    )
    if not row:
        raise HTTPException(404, "not found")
    stats = await db.fetchone(
        "SELECT COUNT(*) AS total, SUM(online) AS online FROM devices WHERE batch_uuid=?",
        (batch_uuid,),
    )
    row["device_count"] = stats["total"] or 0
    row["online_count"] = stats["online"] or 0
    if admin.get("role") == "viewer":
        row.pop("batch_secret", None)
    return row


class RevokeIn(BaseModel):
    reason: str = ""


class BatchPatchIn(BaseModel):
    alias: str | None = None
    notes: str | None = None
    produced_count: int | None = Field(default=None, ge=0)


@router.patch("/{batch_uuid}")
async def patch_batch(batch_uuid: str, body: BatchPatchIn, admin: dict = Depends(A.require_role("admin", "operator"))):
    existing = await db.fetchone("SELECT 1 FROM batches WHERE batch_uuid=? AND deleted_at IS NULL", (batch_uuid,))
    if not existing:
        raise HTTPException(404, "not found")
    sets, vals = [], []
    if body.alias is not None: sets.append("alias=?"); vals.append(body.alias)
    if body.notes is not None: sets.append("notes=?"); vals.append(body.notes)
    if body.produced_count is not None: sets.append("produced_count=?"); vals.append(body.produced_count)
    if not sets:
        return {"ok": True}
    sets.append("updated_at=?"); vals.append(_now())
    vals.append(batch_uuid)
    await db.execute(f"UPDATE batches SET {', '.join(sets)} WHERE batch_uuid=?", tuple(vals))
    updated = await db.fetchone(
        "SELECT batch_uuid, alias, produced_count, revoked, revoked_at, revoked_reason, notes, created_at, updated_at "
        "FROM batches WHERE batch_uuid=?",
        (batch_uuid,),
    )
    await audit.log(actor_type="admin", actor=admin["username"], action="batch_patch",
                    target_kind="batch", target_value=batch_uuid,
                    detail=body.model_dump(exclude_none=True))
    if updated:
        events.emit("batch.updated", **updated)
    return {"ok": True}


@router.post("/{batch_uuid}/revoke")
async def revoke_batch(batch_uuid: str, body: RevokeIn, admin: dict = Depends(A.require_role("admin", "operator"))):
    existing = await db.fetchone("SELECT revoked FROM batches WHERE batch_uuid=? AND deleted_at IS NULL", (batch_uuid,))
    if not existing:
        raise HTTPException(404, "not found")
    if existing["revoked"]:
        return {"ok": True}
    now = _now()
    async with db.begin() as c:
        await c.execute(
            "UPDATE batches SET revoked=1, revoked_at=?, revoked_reason=?, updated_at=? "
            "WHERE batch_uuid=?", (now, body.reason, now, batch_uuid),
        )
        await c.execute(
            "UPDATE devices SET enabled=0, online=0, updated_at=? WHERE batch_uuid=?",
            (now, batch_uuid),
        )
    events.emit("batch.revoked", batch_uuid=batch_uuid, revoked=1, revoked_at=now, revoked_reason=body.reason, updated_at=now)
    await audit.log(actor_type="admin", actor=admin["username"], action="batch_revoke",
                    target_kind="batch", target_value=batch_uuid, detail={"reason": body.reason})
    # Revocation must terminate current MQTT sessions immediately; otherwise an already
    # connected device would keep the session until its next reconnect.
    asyncio.create_task(broker_control.restart_mosquitto(f"batch_revoke:{batch_uuid}"))
    return {"ok": True}


@router.post("/{batch_uuid}/delete")
async def delete_batch(batch_uuid: str, admin: dict = Depends(A.require_role("admin", "operator"))):
    existing = await db.fetchone("SELECT revoked FROM batches WHERE batch_uuid=? AND deleted_at IS NULL", (batch_uuid,))
    if not existing:
        raise HTTPException(404, "not found")
    if not existing["revoked"]:
        raise HTTPException(400, "batch must be revoked first")
    device_rows = await db.fetchall("SELECT device_id FROM devices WHERE batch_uuid=?", (batch_uuid,))
    device_ids = [row["device_id"] for row in device_rows]
    async with db.begin() as conn:
        await _delete_batch_devices(conn, device_ids)
        await conn.execute("DELETE FROM devices WHERE batch_uuid=?", (batch_uuid,))
        await conn.execute("DELETE FROM batches WHERE batch_uuid=?", (batch_uuid,))
    events.emit("batch.deleted", batch_uuid=batch_uuid, deleted_devices=len(device_ids), ts=_now())
    await audit.log(actor_type="admin", actor=admin["username"], action="batch_delete",
                    target_kind="batch", target_value=batch_uuid,
                    detail={"deleted_devices": len(device_ids)})
    return {"ok": True}


@router.post("/{batch_uuid}/restore")
async def restore_batch(batch_uuid: str, admin: dict = Depends(A.require_role("admin", "operator"))):
    existing = await db.fetchone("SELECT revoked FROM batches WHERE batch_uuid=? AND deleted_at IS NULL", (batch_uuid,))
    if not existing:
        raise HTTPException(404, "not found")
    if not existing["revoked"]:
        return {"ok": True}
    now = _now()
    async with db.begin() as conn:
        await conn.execute(
            "UPDATE batches SET revoked=0, revoked_at=NULL, revoked_reason='', updated_at=? WHERE batch_uuid=?",
            (now, batch_uuid),
        )
        await conn.execute(
            "UPDATE devices SET enabled=1, updated_at=? WHERE batch_uuid=?",
            (now, batch_uuid),
        )
    events.emit("batch.restored", batch_uuid=batch_uuid, revoked=0, revoked_at=None, revoked_reason="", updated_at=now)
    await audit.log(actor_type="admin", actor=admin["username"], action="batch_restore",
                    target_kind="batch", target_value=batch_uuid)
    return {"ok": True}


@router.post("/{batch_uuid}/secret")
async def rotate_secret(batch_uuid: str, admin: dict = Depends(A.require_role("admin", "operator"))):
    existing = await db.fetchone("SELECT revoked FROM batches WHERE batch_uuid=? AND deleted_at IS NULL", (batch_uuid,))
    if not existing:
        raise HTTPException(404, "not found")
    if existing["revoked"]:
        raise HTTPException(400, "batch revoked")
    new_secret = secrets.token_hex(32)
    now = _now()
    await db.execute(
        "UPDATE batches SET batch_secret=?, updated_at=? WHERE batch_uuid=?",
        (new_secret, now, batch_uuid),
    )
    events.emit("batch.updated", batch_uuid=batch_uuid, updated_at=now)
    await audit.log(actor_type="admin", actor=admin["username"], action="batch_rotate_secret",
                    target_kind="batch", target_value=batch_uuid)
    return {"batch_uuid": batch_uuid, "batch_secret": new_secret}
