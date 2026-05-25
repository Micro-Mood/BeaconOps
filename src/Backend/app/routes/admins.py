"""管理员账号增删改 + 审计查询。"""
from __future__ import annotations
import time

import bcrypt
from fastapi import APIRouter, Depends, HTTPException, Query
from pydantic import BaseModel

from .. import audit
from .. import auth as A
from .. import database as db
from .. import events

router = APIRouter()


class AdminCreateIn(BaseModel):
    username: str
    password: str
    role: str = "operator"  # admin | operator | viewer


@router.get("")
async def list_admins(admin: dict = Depends(A.require_role("admin"))):
    rows = await db.fetchall(
        "SELECT username, role, enabled, created_at, last_login_at "
        "FROM admins ORDER BY enabled DESC, created_at DESC, username"
    )
    return {"admins": rows}


@router.post("")
async def create_admin(body: AdminCreateIn, admin: dict = Depends(A.require_role("admin"))):
    if not body.username.strip():
        raise HTTPException(400, "username required")
    if not body.password:
        raise HTTPException(400, "password required")
    if body.role not in ("admin", "operator", "viewer"):
        raise HTTPException(400, "bad role")
    existing = await db.fetchone("SELECT 1 FROM admins WHERE username=?", (body.username,))
    if existing:
        raise HTTPException(409, "exists")
    pw = bcrypt.hashpw(body.password.encode(), bcrypt.gensalt()).decode()
    await db.execute(
        "INSERT INTO admins(username, password_hash, role, enabled, created_at) "
        "VALUES(?,?,?,1,?)",
        (body.username, pw, body.role, int(time.time())),
    )
    events.emit("admin.created", username=body.username, role=body.role, enabled=1)
    await audit.log(actor_type="admin", actor=admin["username"], action="admin_create",
                    target_kind="admin", target_value=body.username,
                    detail={"role": body.role})
    return {"ok": True}


class AdminPatchIn(BaseModel):
    password: str | None = None
    role: str | None = None
    enabled: bool | None = None


@router.patch("/{username}")
async def patch_admin(username: str, body: AdminPatchIn, admin: dict = Depends(A.require_role("admin"))):
    existing = await db.fetchone("SELECT role, enabled FROM admins WHERE username=?", (username,))
    if not existing:
        raise HTTPException(404, "not found")
    sets, vals = [], []
    if body.password is not None:
        sets.append("password_hash=?")
        vals.append(bcrypt.hashpw(body.password.encode(), bcrypt.gensalt()).decode())
    if body.role is not None:
        if body.role not in ("admin", "operator", "viewer"):
            raise HTTPException(400, "bad role")
        if username == admin["username"] and body.role != "admin":
            raise HTTPException(400, "cannot demote yourself")
        sets.append("role=?"); vals.append(body.role)
    if body.enabled is not None:
        if username == admin["username"] and not body.enabled:
            raise HTTPException(400, "cannot disable yourself")
        sets.append("enabled=?"); vals.append(1 if body.enabled else 0)
    if not sets:
        return {"ok": True}
    vals.append(username)
    await db.execute(f"UPDATE admins SET {', '.join(sets)} WHERE username=?", tuple(vals))
    updated = await db.fetchone(
        "SELECT username, role, enabled, created_at, last_login_at FROM admins WHERE username=?",
        (username,),
    )
    await audit.log(actor_type="admin", actor=admin["username"], action="admin_patch",
                    target_kind="admin", target_value=username,
                    detail=body.model_dump(exclude_none=True))
    if updated:
        events.emit("admin.updated", **updated)
    return {"ok": True}


@router.delete("/{username}")
async def delete_admin(username: str, admin: dict = Depends(A.require_role("admin"))):
    if username == admin["username"]:
        raise HTTPException(400, "cannot delete yourself")
    existing = await db.fetchone("SELECT 1 FROM admins WHERE username=?", (username,))
    if not existing:
        raise HTTPException(404, "not found")
    await db.execute("UPDATE admins SET enabled=0 WHERE username=?", (username,))
    events.emit("admin.deleted", username=username, enabled=0)
    await audit.log(actor_type="admin", actor=admin["username"], action="admin_delete",
                    target_kind="admin", target_value=username)
    return {"ok": True}
