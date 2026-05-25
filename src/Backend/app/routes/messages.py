"""消息下发/查询。"""
from __future__ import annotations
import time

from fastapi import APIRouter, Depends, HTTPException, Query
from pydantic import BaseModel, Field

from .. import audit
from .. import auth as A
from .. import database as db
from .. import messages as msg_svc

router = APIRouter()


class SendIn(BaseModel):
    target_kind:  str = Field(pattern="^(device|all|dept)$")
    target_value: str = ""
    level:        str = Field(default="info", pattern="^(info|notice|warn|emergency)$")
    title:        str = ""
    body:         str = ""
    ttl:          int = 3600
    ack_mode:     str = Field(default="received", pattern="^(none|received|displayed|acknowledged)$")


@router.post("")
async def send_message(body: SendIn, admin: dict = Depends(A.require_role("admin", "operator"))):
    try:
        result = await msg_svc.create_message(
            target_kind=body.target_kind, target_value=body.target_value,
            level=body.level, title=body.title, body=body.body,
            ttl=body.ttl, ack_mode=body.ack_mode,
            source_type="admin", operator=admin["username"],
        )
    except ValueError as e:
        raise HTTPException(400, str(e))
    await audit.log(actor_type="admin", actor=admin["username"], action="send_message",
                    target_kind=body.target_kind, target_value=body.target_value,
                    detail={**body.model_dump(), "msg_id": result["msg_id"]})
    return result


@router.get("")
async def list_messages(
    admin: dict = Depends(A.current_admin),
    status:      str | None = Query(None),
    level:       str | None = Query(None),
    target_kind: str | None = Query(None),
    since:       int = Query(0),
    until:       int = Query(0),
    limit:       int = Query(100, ge=1, le=1000),
    offset:      int = Query(0, ge=0),
):
    where = ["1=1"]
    params: list = []
    if status:      where.append("m.status=?");      params.append(status)
    if level:       where.append("m.level=?");       params.append(level)
    if target_kind: where.append("m.target_kind=?"); params.append(target_kind)
    if since:       where.append("m.created_at>=?"); params.append(since)
    if until:       where.append("m.created_at<=?"); params.append(until)
    where_sql = " AND ".join(where)
    total = await db.fetchone(f"SELECT COUNT(*) AS n FROM messages m WHERE {where_sql}", tuple(params))
    rows = await db.fetchall(
        f"SELECT {msg_svc.select_columns('m')} FROM messages m WHERE {where_sql} "
        "ORDER BY m.created_at DESC, m.msg_id DESC LIMIT ? OFFSET ?",
        tuple(params + [limit, offset]),
    )
    return {"messages": rows, "total": (total or {}).get("n", len(rows))}


@router.get("/{msg_id}")
async def get_message(msg_id: str, admin: dict = Depends(A.current_admin)):
    row = await db.fetchone(
        f"SELECT {msg_svc.select_columns('m')} FROM messages m WHERE m.msg_id=?", (msg_id,),
    )
    if not row:
        raise HTTPException(404, "not found")
    return row


@router.get("/{msg_id}/recipients")
async def get_recipients(msg_id: str, admin: dict = Depends(A.current_admin)):
    rows = await db.fetchall(
        "SELECT r.*, d.alias, d.dept, '' AS roles_json FROM message_recipients r "
        "LEFT JOIN devices d ON d.device_id=r.device_id "
        "WHERE r.msg_id=? ORDER BY d.dept, COALESCE(NULLIF(d.alias, ''), r.device_id), r.device_id",
        (msg_id,),
    )
    return {"recipients": rows}
