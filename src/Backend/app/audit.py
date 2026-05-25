"""审计日志记录。"""
from __future__ import annotations
import json
import time

from . import database as db
from . import events


async def log(*, actor_type: str, actor: str, action: str,
              target_kind: str = "", target_value: str = "",
              detail: dict | None = None, ip: str = "") -> None:
    ts = int(time.time())
    detail_json = json.dumps(detail or {}, ensure_ascii=False)
    row_id = await db.execute(
        "INSERT INTO audit_log(ts, actor_type, actor, action, target_kind, target_value, "
        "                      detail_json, ip) VALUES(?,?,?,?,?,?,?,?)",
        (ts, actor_type, actor, action,
         target_kind, target_value,
         detail_json, ip),
    )
    events.emit(
        "audit.created",
        id=row_id,
        ts=ts,
        actor_type=actor_type,
        actor=actor,
        action=action,
        target_kind=target_kind,
        target_value=target_value,
        detail_json=detail_json,
        ip=ip,
    )
