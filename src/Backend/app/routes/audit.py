"""审计日志查询。"""
from __future__ import annotations
from fastapi import APIRouter, Depends, Query

from .. import auth as A
from .. import database as db

router = APIRouter()


@router.get("")
async def list_audit(
    admin: dict = Depends(A.require_role("admin")),
    actor:  str | None = Query(None),
    action: str | None = Query(None),
    since:  int = Query(0),
    until:  int = Query(0),
    limit:  int = Query(200, ge=1, le=2000),
):
    where = ["1=1"]; params: list = []
    if actor:  where.append("actor=?");  params.append(actor)
    if action: where.append("action=?"); params.append(action)
    if since:  where.append("ts>=?");    params.append(since)
    if until:  where.append("ts<=?");    params.append(until)
    sql = f"SELECT * FROM audit_log WHERE {' AND '.join(where)} ORDER BY id DESC LIMIT ?"
    params.append(limit)
    rows = await db.fetchall(sql, tuple(params))
    return {"audit": rows, "total": len(rows)}
