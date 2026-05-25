"""BeaconOps FastAPI 主应用。"""
from __future__ import annotations
import asyncio
import hmac
import logging
import time
from contextlib import asynccontextmanager
from logging.handlers import RotatingFileHandler
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.middleware.gzip import GZipMiddleware
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

from . import auth as A
from . import config
from . import database as db
from . import events
from . import messages as msg_svc
from . import mqtt_client
from . import uplink_handler
from .routes.admins    import router as admins_router
from .routes.audit     import router as audit_router
from .routes.auth      import router as auth_router
from .routes.batches   import router as batches_router
from .routes.devices   import router as devices_router
from .routes.messages  import router as messages_router
from .routes.stream    import router as stream_router

config.DATA_DIR.mkdir(parents=True, exist_ok=True)
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(levelname)s  %(message)s",
    handlers=[
        RotatingFileHandler(
            config.DATA_DIR / "server.log", encoding="utf-8",
            maxBytes=10 * 1024 * 1024, backupCount=3,
        ),
        logging.StreamHandler(),
    ],
)
log = logging.getLogger("beacon")
CONSOLE_DIR = Path(__file__).resolve().parent.parent / "console"

_retry_task: asyncio.Task | None = None
_cleanup_task: asyncio.Task | None = None


async def _cleanup_loop():
    while True:
        try:
            await asyncio.sleep(600)
            n = await db.cleanup_expired_nonces()
            if n:
                log.info("cleaned %d expired nonces/tokens", n)
        except asyncio.CancelledError:
            return
        except Exception:
            log.exception("cleanup_loop")


@asynccontextmanager
async def lifespan(_app: FastAPI):
    global _retry_task, _cleanup_task
    await db.init()
    mqtt_client.set_uplink_handler(uplink_handler.dispatch)
    try:
        await mqtt_client.start()
    except Exception:
        log.exception("MQTT start failed; will run without bridge")
    _retry_task = asyncio.create_task(msg_svc.retry_loop())
    _cleanup_task = asyncio.create_task(_cleanup_loop())
    log.info("BeaconOps started")
    try:
        yield
    finally:
        log.info("shutdown...")
        for t in (_retry_task, _cleanup_task):
            if t:
                t.cancel()
                try:
                    await t
                except (asyncio.CancelledError, Exception):
                    pass
        await mqtt_client.stop()
        await db.close()


app = FastAPI(title="BeaconOps", version="1.0.0", lifespan=lifespan)
app.add_middleware(GZipMiddleware, minimum_size=1024)

V1 = "/beacon/api/v1"


@app.middleware("http")
async def csrf_guard(request: Request, call_next):
    unsafe = request.method.upper() in {"POST", "PUT", "PATCH", "DELETE"}
    path = request.url.path
    csrf_exempt = path in {f"{V1}/auth/login", f"{V1}/auth/device"}
    bearer = request.headers.get("authorization", "").startswith("Bearer ")
    if unsafe and path.startswith(V1) and not csrf_exempt and not bearer:
        session = request.cookies.get(A.COOKIE_NAME, "")
        expected = A.csrf_from_token(session) if session else None
        cookie = request.cookies.get(A.CSRF_COOKIE_NAME, "")
        header = request.headers.get(A.CSRF_HEADER_NAME, "")
        if not (expected and cookie and header and
                hmac.compare_digest(expected, cookie) and
                hmac.compare_digest(expected, header)):
            return JSONResponse({"detail": "bad csrf token"}, 403)
    return await call_next(request)

app.include_router(auth_router,     prefix=f"{V1}/auth",     tags=["auth"])
app.include_router(batches_router,  prefix=f"{V1}/batches",  tags=["batches"])
app.include_router(devices_router,  prefix=f"{V1}/devices",  tags=["devices"])
app.include_router(messages_router,  prefix=f"{V1}/messages",  tags=["messages"])
app.include_router(admins_router,    prefix=f"{V1}/admins",    tags=["admins"])
app.include_router(audit_router,    prefix=f"{V1}/audit",    tags=["audit"])
app.include_router(stream_router,   prefix=f"{V1}/stream",   tags=["stream"])


@app.get("/beacon/health")
async def health():
    return {
        "ok": True,
        "version": app.version,
        "environment": config.ENVIRONMENT,
        "time": int(time.time()),
        "mqtt": mqtt_client.is_connected(),
    }


_NO_CACHE = {"Cache-Control": "no-cache, no-store, must-revalidate", "Pragma": "no-cache"}

if CONSOLE_DIR.exists():
    if (CONSOLE_DIR / "assets").exists():
        app.mount("/beacon/console/assets",
                  StaticFiles(directory=str(CONSOLE_DIR / "assets")), name="console-assets")

    @app.get("/beacon/console")
    @app.get("/beacon/console/")
    @app.get("/beacon/console/{path:path}")
    async def console_spa(path: str = ""):
        if path:
            f = CONSOLE_DIR / path
            if f.is_file():
                return FileResponse(str(f))
        idx = CONSOLE_DIR / "index.html"
        if not idx.exists():
            return JSONResponse({"error": "console not built"}, 404)
        return FileResponse(str(idx), headers=_NO_CACHE)


@app.get("/beacon/")
@app.get("/beacon")
async def root():
    return RedirectResponse("/beacon/console/")


@app.exception_handler(Exception)
async def _all_exc(request: Request, exc: Exception):
    if isinstance(exc, asyncio.CancelledError):
        raise
    log.exception("unhandled: %s %s", request.method, request.url.path)
    return JSONResponse({"error": "internal"}, 500)
