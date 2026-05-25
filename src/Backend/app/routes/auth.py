"""POST /auth/login + GET /auth/me + POST /auth/device(broker webhook)。"""
from __future__ import annotations
from ipaddress import ip_address
import re
import time

from fastapi import APIRouter, Depends, HTTPException, Request, Response, Header, Cookie
from pydantic import BaseModel

from .. import auth as A
from .. import audit
from .. import config
from .. import database as db
from .. import events
from .. import identity

router = APIRouter()

_BRIDGE_CMD_RE = re.compile(r"^device/[0-9a-f]{12}/cmd$")
_BRIDGE_SUB_TOPICS = {
    "device/+/status",
    "device/+/uplink/ack",
    "device/+/uplink/event",
    "device/+/uplink/health",
    "device/+/uplink/profile",
}


class LoginIn(BaseModel):
    username: str
    password: str


@router.post("/login")
async def login(body: LoginIn, response: Response, request: Request):
    admin = await A.verify_password(body.username, body.password)
    if not admin:
        await audit.log(actor_type="anon", actor=body.username, action="login_fail",
                        ip=_client_ip(request))
        raise HTTPException(401, "bad credentials")
    token = A.make_token(admin["username"])
    csrf = A.csrf_from_token(token)
    response.set_cookie(A.COOKIE_NAME, token, max_age=config.JWT_TTL_S,
                        httponly=True, samesite="lax")
    response.set_cookie(A.CSRF_COOKIE_NAME, csrf or "", max_age=config.JWT_TTL_S,
                        httponly=False, samesite="lax")
    last_login_at = int(time.time())
    await db.execute("UPDATE admins SET last_login_at=? WHERE username=?",
                     (last_login_at, admin["username"]))
    events.emit("admin.login", username=admin["username"], role=admin["role"], last_login_at=last_login_at)
    await audit.log(actor_type="admin", actor=admin["username"], action="login",
                    ip=_client_ip(request))
    return {"username": admin["username"], "role": admin["role"]}


@router.post("/logout")
async def logout(response: Response, admin: dict = Depends(A.current_admin)):
    response.delete_cookie(A.COOKIE_NAME)
    response.delete_cookie(A.CSRF_COOKIE_NAME)
    return {"ok": True}


@router.get("/me")
async def me(response: Response, beacon_session: str | None = Cookie(None), admin: dict = Depends(A.current_admin)):
    if beacon_session:
        csrf = A.csrf_from_token(beacon_session)
        if csrf:
            response.set_cookie(A.CSRF_COOKIE_NAME, csrf, max_age=config.JWT_TTL_S,
                                httponly=False, samesite="lax")
    return {"username": admin["username"], "role": admin["role"]}


# ── Broker auth webhook(mosquitto-go-auth HTTP backend) ──
# 期望调用方:
#   POST /api/v1/auth/device
#   body: { "clientid": "...", "username": "...", "password": "...", "acc": 1 }
# 返回兼容 mosquitto-go-auth JSON response mode:
#   {"ok": true, "error": "", "result": "allow"}
#   {"ok": false, "error": "reason", "result": "deny"}
# ACL 校验:同 endpoint,通过 acc 字段 (1=read/sub, 2=write/pub) 与 topic
# 区分;mosquitto-go-auth 在 backend "http" + check_method "POST" 下都会
# 调一次 /auth/device 接 user/superuser/acl 三种,实现侧统一用 mode。

class DeviceAuthIn(BaseModel):
    clientid: str = ""
    username: str = ""
    password: str = ""
    topic: str = ""
    acc: int = 0   # 0=user check, 1=sub, 2=pub, 3=subscribe, 4=write


def _auth_result(ok: bool, reason: str = "") -> dict:
    normalized_reason = "" if ok else reason
    return {
        "ok": ok,
        "error": normalized_reason,
        "result": "allow" if ok else "deny",
        "reason": normalized_reason,
    }


def _verify_bridge_connect(client_id: str, username: str, password: str) -> tuple[bool, str]:
    if not config.MQTT_USERNAME or not config.MQTT_PASSWORD:
        return False, "bridge_auth_not_configured"
    if client_id != config.MQTT_CLIENT_ID:
        return False, "bridge_bad_client_id"
    if username != config.MQTT_USERNAME:
        return False, "bridge_bad_username"
    if password != config.MQTT_PASSWORD:
        return False, "bridge_bad_password"
    return True, "ok"


def _bridge_acl_ok(topic: str, wanted_access: str) -> bool:
    if wanted_access == "sub":
        return topic in _BRIDGE_SUB_TOPICS
    return bool(_BRIDGE_CMD_RE.match(topic))


def _wanted_access_for_mode(mode: int) -> str | None:
    # Mosquitto ACL acc: 1=read, 2=write, 4=subscribe.
    # 对设备侧语义来说, read/subscribe 都对应订阅 cmd 类主题, write 对应上行 publish。
    if mode in (1, 3, 4):
        return "sub"
    if mode == 2:
        return "pub"
    return None


@router.post("/device")
async def device_auth(
    request: Request,
    body: DeviceAuthIn,
    x_webhook_token: str | None = Header(default=None, alias="X-Webhook-Token"),
):
    # 共享令牌校验。未配置 token 时只允许本机直连,避免公网经 nginx 旁路调用。
    if config.AUTH_WEBHOOK_TOKEN:
        if x_webhook_token != config.AUTH_WEBHOOK_TOKEN:
            raise HTTPException(403, "bad webhook token")
    elif not _is_loopback_request(request):
        raise HTTPException(403, "webhook token required")

    mode = body.acc

    # 用户名/密码鉴权(mosquitto-go-auth "user check")
    if mode in (0,):
        if body.username == config.MQTT_USERNAME:
            ok, reason = _verify_bridge_connect(body.clientid, body.username, body.password)
            return _auth_result(ok, reason)
        ok, reason = await identity.verify_connect(body.clientid, body.username, body.password)
        return _auth_result(ok, reason)

    # ACL 检查(sub/pub)
    if mode in (1, 2, 3, 4):
        wanted_access = _wanted_access_for_mode(mode)
        if wanted_access is None:
            return _auth_result(False, "unknown_mode")
        if body.username == config.MQTT_USERNAME:
            return _auth_result(_bridge_acl_ok(body.topic or "", wanted_access), "acl_no_match")
        device_id = (body.clientid or "").strip().lower()
        topic = body.topic or ""
        acl = await identity.build_acl(device_id)
        for rule in acl:
            if rule["topic"] == topic and rule["access"] == wanted_access:
                return _auth_result(True)
        return _auth_result(False, "acl_no_match")

    # superuser check 永远 deny(没有超级设备)
    return _auth_result(False, "unknown_mode")


def _client_ip(request: Request) -> str:
    forwarded_parts = [part.strip() for part in request.headers.get("x-forwarded-for", "").split(",") if part.strip()]
    forwarded = forwarded_parts[-1] if forwarded_parts else ""
    return forwarded or (request.client.host if request.client else "")


def _is_loopback_request(request: Request) -> bool:
    forwarded_parts = [part.strip() for part in request.headers.get("x-forwarded-for", "").split(",") if part.strip()]
    if forwarded_parts:
        try:
            return all(ip_address(part).is_loopback for part in forwarded_parts)
        except ValueError:
            return False
    host = request.client.host if request.client else ""
    try:
        return ip_address(host).is_loopback
    except ValueError:
        return False
