"""设备管理。"""
from __future__ import annotations
import time

from fastapi import APIRouter, Depends, HTTPException, Query
from pydantic import BaseModel

from .. import audit
from .. import auth as A
from .. import database as db
from .. import events
from .. import messages as msg_svc

router = APIRouter()


def _now() -> int:
    return int(time.time())


@router.get("")
async def list_devices(
    admin: dict = Depends(A.current_admin),
    batch:   str | None = Query(None),
    dept:    str | None = Query(None),
    online:  int | None = Query(None),
    enabled: int | None = Query(None),
    q:       str | None = Query(None),
):
    where = ["1=1"]
    params: list = []
    if batch:
        where.append("batch_uuid=?"); params.append(batch)
    if dept:
        where.append("dept=?"); params.append(dept)
    if online is not None:
        where.append("online=?"); params.append(int(bool(online)))
    if enabled is not None:
        where.append("enabled=?"); params.append(int(bool(enabled)))
    if q:
        where.append("(device_id LIKE ? OR alias LIKE ?)")
        params.extend([f"%{q}%", f"%{q}%"])
    sql = f"SELECT * FROM devices WHERE {' AND '.join(where)} ORDER BY online DESC, last_seen_at DESC, device_id"
    rows = [_shape_device(row) for row in await db.fetchall(sql, tuple(params))]
    return {"devices": rows, "total": len(rows)}


@router.get("/{device_id}")
async def get_device(device_id: str, admin: dict = Depends(A.current_admin)):
    row = await db.fetchone("SELECT * FROM devices WHERE device_id=?", (device_id,))
    if not row:
        raise HTTPException(404, "not found")
    return _shape_device(row)


class DevicePatchIn(BaseModel):
    alias: str | None = None
    dept:  str | None = None


@router.patch("/{device_id}")
async def patch_device(device_id: str, body: DevicePatchIn, admin: dict = Depends(A.require_role("admin", "operator"))):
    existing = await db.fetchone("SELECT 1 FROM devices WHERE device_id=?", (device_id,))
    if not existing:
        raise HTTPException(404, "not found")
    sets, vals = [], []
    if body.alias   is not None: sets.append("alias=?"); vals.append(body.alias)
    if body.dept    is not None: sets.append("dept=?");  vals.append(body.dept)
    if not sets:
        return {"ok": True}
    sets.append("updated_at=?"); vals.append(_now())
    vals.append(device_id)
    await db.execute(f"UPDATE devices SET {', '.join(sets)} WHERE device_id=?", tuple(vals))
    updated = await db.fetchone("SELECT * FROM devices WHERE device_id=?", (device_id,))
    await audit.log(actor_type="admin", actor=admin["username"], action="device_patch",
                    target_kind="device", target_value=device_id,
                    detail=body.model_dump(exclude_none=True))
    if updated:
        events.emit(
            "device.updated",
            device_id=device_id,
            batch_uuid=updated["batch_uuid"],
            alias=updated["alias"],
            dept=updated["dept"],
            enabled=updated["enabled"],
            online=updated["online"],
            updated_at=updated["updated_at"],
        )
    return {"ok": True}


@router.get("/{device_id}/messages")
async def device_messages(device_id: str, admin: dict = Depends(A.current_admin),
                          limit: int = Query(50, ge=1, le=500)):
    rows = await db.fetchall(
        f"SELECT {msg_svc.select_columns('m')} FROM messages m "
        "JOIN message_recipients r ON r.msg_id=m.msg_id "
        "WHERE r.device_id=? ORDER BY m.created_at DESC, m.msg_id DESC LIMIT ?",
        (device_id, limit),
    )
    return {"messages": rows}


@router.get("/{device_id}/behavior")
async def device_behavior(
    device_id: str,
    admin: dict = Depends(A.current_admin),
    since: int = Query(..., ge=0),
    until: int = Query(..., ge=0),
    bucket_s: int = Query(300, ge=60, le=7200),
):
    if until <= since:
        raise HTTPException(400, "invalid range")
    if bucket_s % 60 != 0:
        raise HTTPException(400, "bucket_s must be multiple of 60")

    existing = await db.fetchone("SELECT 1 FROM devices WHERE device_id=?", (device_id,))
    if not existing:
        raise HTTPException(404, "not found")

    bucket_expr = "(? + CAST((ts - ?) / ? AS INTEGER) * ?)"
    rows = await db.fetchall(
        f"SELECT {bucket_expr} AS bucket_ts, "
        "       SUM(static_s) AS static_s, "
        "       SUM(walk_slow_s) AS walk_slow_s, "
        "       SUM(walk_fast_s) AS walk_fast_s, "
        "       SUM(run_s) AS run_s, "
        "       SUM(shake_or_fall_s) AS shake_or_fall_s "
        "FROM behavior "
        "WHERE device_id=? AND ts>=? AND ts<? "
        "GROUP BY bucket_ts ORDER BY bucket_ts",
        (since, since, bucket_s, bucket_s, device_id, since, until),
    )

    series_by_ts = {
        int(row["bucket_ts"]): {
            "static_s": int(row["static_s"] or 0),
            "walk_slow_s": int(row["walk_slow_s"] or 0),
            "walk_fast_s": int(row["walk_fast_s"] or 0),
            "run_s": int(row["run_s"] or 0),
            "shake_or_fall_s": int(row["shake_or_fall_s"] or 0),
        }
        for row in rows
    }

    points = []
    bucket_ts = since
    while bucket_ts < until:
        point = series_by_ts.get(bucket_ts, {})
        points.append({
            "ts": bucket_ts,
            "static_s": int(point.get("static_s", 0)),
            "walk_slow_s": int(point.get("walk_slow_s", 0)),
            "walk_fast_s": int(point.get("walk_fast_s", 0)),
            "run_s": int(point.get("run_s", 0)),
            "shake_or_fall_s": int(point.get("shake_or_fall_s", 0)),
        })
        bucket_ts += bucket_s

    return {
        "since": since,
        "until": until,
        "bucket_s": bucket_s,
        "points": points,
    }


def _shape_device(row: dict) -> dict:
    battery_soc = row.get("battery_soc", -1)
    row["battery"] = None if battery_soc is None or battery_soc < 0 else battery_soc
    row["ip"] = row.get("last_ip") or None
    row["uptime"] = row.get("uptime_s") or None
    row["roles_json"] = ""
    row["roles"] = []
    return row
