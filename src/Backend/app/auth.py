"""管理员认证 — JWT cookie。"""
from __future__ import annotations
import logging
import secrets
import time
from typing import Optional

import bcrypt
import jwt
from fastapi import Cookie, Depends, HTTPException, Request

from . import config
from . import database as db

COOKIE_NAME = "beacon_session"
CSRF_COOKIE_NAME = "beacon_csrf"
CSRF_HEADER_NAME = "x-csrf-token"
log = logging.getLogger("beacon.auth")


def make_token(username: str) -> str:
    now = int(time.time())
    return jwt.encode(
        {"sub": username, "csrf": secrets.token_urlsafe(32), "iat": now, "exp": now + config.JWT_TTL_S},
        config.JWT_SECRET, algorithm=config.JWT_ALG,
    )


def _decode_token(token: str) -> dict | None:
    try:
        return jwt.decode(token, config.JWT_SECRET, algorithms=[config.JWT_ALG])
    except jwt.PyJWTError:
        return None
    except Exception:
        log.exception("unexpected token decode error")
        return None


def parse_token(token: str) -> Optional[str]:
    data = _decode_token(token)
    if not data or not data.get("csrf"):
        return None
    return data.get("sub")


def csrf_from_token(token: str) -> str | None:
    data = _decode_token(token)
    csrf = data.get("csrf") if data else None
    return csrf if isinstance(csrf, str) and csrf else None


async def verify_password(username: str, password: str) -> Optional[dict]:
    admin = await db.fetchone(
        "SELECT * FROM admins WHERE username=? AND enabled=1", (username,)
    )
    if not admin:
        return None
    try:
        ok = bcrypt.checkpw(password.encode(), admin["password_hash"].encode())
    except (TypeError, ValueError):
        return None
    except Exception:
        log.exception("unexpected password verify error for %s", username)
        return None
    return admin if ok else None


async def current_admin(
    request: Request,
    beacon_session: Optional[str] = Cookie(None),
) -> dict:
    token = beacon_session
    if not token:
        auth = request.headers.get("authorization", "")
        if auth.startswith("Bearer "):
            token = auth[7:]
    if not token:
        raise HTTPException(401, "not authenticated")
    username = parse_token(token)
    if not username:
        raise HTTPException(401, "invalid token")
    admin = await db.fetchone(
        "SELECT * FROM admins WHERE username=? AND enabled=1", (username,)
    )
    if not admin:
        raise HTTPException(401, "admin not found")
    return admin


def require_role(*roles: str):
    allowed = set(roles)

    async def _dep(admin: dict = Depends(current_admin)) -> dict:
        if admin.get("role") not in allowed:
            raise HTTPException(403, "insufficient role")
        return admin

    return _dep
