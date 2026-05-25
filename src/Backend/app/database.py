"""SQLite 持久层(WAL)— BeaconOps data-model v1。

表清单见 BeaconOps/落地/data-model-v1.md。
"""
from __future__ import annotations
import asyncio
from contextlib import asynccontextmanager
import json
import logging
import time
from pathlib import Path

import aiosqlite

from . import config

log = logging.getLogger("beacon.db")

_db: aiosqlite.Connection | None = None
_write_lock = asyncio.Lock()


SCHEMA = """
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS batches (
    batch_uuid       TEXT PRIMARY KEY,
    alias            TEXT NOT NULL DEFAULT '',
    batch_secret     TEXT NOT NULL,
    produced_at      INTEGER NOT NULL,
    produced_count   INTEGER NOT NULL DEFAULT 0,
    revoked          INTEGER NOT NULL DEFAULT 0,
    revoked_at       INTEGER,
    revoked_reason   TEXT NOT NULL DEFAULT '',
    notes            TEXT NOT NULL DEFAULT '',
    deleted_at       INTEGER,
    created_at       INTEGER NOT NULL,
    updated_at       INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_batches_revoked ON batches(revoked);

CREATE TABLE IF NOT EXISTS devices (
    device_id        TEXT PRIMARY KEY,
    batch_uuid       TEXT NOT NULL,
    alias            TEXT NOT NULL DEFAULT '',
    dept             TEXT NOT NULL DEFAULT '',
    enabled          INTEGER NOT NULL DEFAULT 1,
    online           INTEGER NOT NULL DEFAULT 0,
    fw_version       TEXT NOT NULL DEFAULT '',
    battery_soc      INTEGER NOT NULL DEFAULT -1,
    charging         INTEGER NOT NULL DEFAULT 0,
    rssi             INTEGER NOT NULL DEFAULT 0,
    last_ip          TEXT NOT NULL DEFAULT '',
    uptime_s         INTEGER NOT NULL DEFAULT 0,
    spiffs_pending   INTEGER NOT NULL DEFAULT 0,
    ack_pending      INTEGER NOT NULL DEFAULT 0,
    drop_count       INTEGER NOT NULL DEFAULT 0,
    first_seen_at    INTEGER,
    last_seen_at     INTEGER,
    last_health_at   INTEGER,
    created_at       INTEGER NOT NULL,
    updated_at       INTEGER NOT NULL,
    FOREIGN KEY (batch_uuid) REFERENCES batches(batch_uuid)
);
CREATE INDEX IF NOT EXISTS idx_devices_batch    ON devices(batch_uuid);
CREATE INDEX IF NOT EXISTS idx_devices_dept     ON devices(dept);
CREATE INDEX IF NOT EXISTS idx_devices_online   ON devices(online);
CREATE INDEX IF NOT EXISTS idx_devices_enabled  ON devices(enabled);

CREATE TABLE IF NOT EXISTS messages (
    msg_id           TEXT PRIMARY KEY,
    target_kind      TEXT NOT NULL,           -- device | all | dept
    target_value     TEXT NOT NULL DEFAULT '',
    level            TEXT NOT NULL,           -- info | notice | warn | emergency
    title            TEXT NOT NULL DEFAULT '',
    body             TEXT NOT NULL DEFAULT '',
    ttl              INTEGER NOT NULL DEFAULT 0,
    ack_mode         TEXT NOT NULL,           -- none|received|displayed|acknowledged
    source_type      TEXT NOT NULL,           -- admin | system
    operator         TEXT NOT NULL DEFAULT '',
    status           TEXT NOT NULL,           -- queued|sent|delivered|displayed|acknowledged|expired|failed
    attempt_count    INTEGER NOT NULL DEFAULT 0,
    next_retry_at    INTEGER,
    last_send_at     INTEGER,
    last_error       TEXT NOT NULL DEFAULT '',
    created_at       INTEGER NOT NULL,
    sent_at          INTEGER,
    delivered_at     INTEGER,
    displayed_at     INTEGER,
    acknowledged_at  INTEGER,
    expired_at       INTEGER,
    failed_at        INTEGER,
    payload_json     TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_messages_status     ON messages(status);
CREATE INDEX IF NOT EXISTS idx_messages_target     ON messages(target_kind, target_value);
CREATE INDEX IF NOT EXISTS idx_messages_created    ON messages(created_at);
CREATE INDEX IF NOT EXISTS idx_messages_next_retry ON messages(next_retry_at);

CREATE TABLE IF NOT EXISTS message_recipients (
    msg_id           TEXT NOT NULL,
    device_id        TEXT NOT NULL,
    status           TEXT NOT NULL DEFAULT 'queued',
    attempt_count    INTEGER NOT NULL DEFAULT 0,
    next_retry_at    INTEGER,
    last_send_at     INTEGER,
    delivered_at     INTEGER,
    displayed_at     INTEGER,
    acknowledged_at  INTEGER,
    expired_at       INTEGER,
    failed_at        INTEGER,
    last_error       TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (msg_id, device_id),
    FOREIGN KEY (msg_id)    REFERENCES messages(msg_id),
    FOREIGN KEY (device_id) REFERENCES devices(device_id)
);
CREATE INDEX IF NOT EXISTS idx_recipients_device ON message_recipients(device_id);
CREATE INDEX IF NOT EXISTS idx_recipients_status ON message_recipients(status);

CREATE TABLE IF NOT EXISTS message_events (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    msg_id           TEXT NOT NULL,
    device_id        TEXT NOT NULL,
    kind             TEXT NOT NULL,
    detail           TEXT NOT NULL DEFAULT '',
    ts               INTEGER NOT NULL,
    recorded_at      INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_events_msg     ON message_events(msg_id);
CREATE INDEX IF NOT EXISTS idx_events_device  ON message_events(device_id);
CREATE INDEX IF NOT EXISTS idx_events_kind    ON message_events(kind);
CREATE UNIQUE INDEX IF NOT EXISTS uq_events_dedup ON message_events(msg_id, device_id, kind);

CREATE TABLE IF NOT EXISTS device_health_history (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id        TEXT NOT NULL,
    ts               INTEGER NOT NULL,
    battery_soc      INTEGER,
    charging         INTEGER,
    rssi             INTEGER,
    spiffs_pending   INTEGER,
    ack_pending      INTEGER
);
CREATE INDEX IF NOT EXISTS idx_health_device_ts ON device_health_history(device_id, ts);

CREATE TABLE IF NOT EXISTS admins (
    username         TEXT PRIMARY KEY,
    password_hash    TEXT NOT NULL,
    role             TEXT NOT NULL DEFAULT 'operator',
    enabled          INTEGER NOT NULL DEFAULT 1,
    created_at       INTEGER NOT NULL,
    last_login_at    INTEGER
);

CREATE TABLE IF NOT EXISTS audit_log (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    ts               INTEGER NOT NULL,
    actor_type       TEXT NOT NULL,
    actor            TEXT NOT NULL,
    action           TEXT NOT NULL,
    target_kind      TEXT NOT NULL DEFAULT '',
    target_value     TEXT NOT NULL DEFAULT '',
    detail_json      TEXT NOT NULL DEFAULT '{}',
    ip               TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_audit_ts     ON audit_log(ts);
CREATE INDEX IF NOT EXISTS idx_audit_actor  ON audit_log(actor);
CREATE INDEX IF NOT EXISTS idx_audit_action ON audit_log(action);

CREATE TABLE IF NOT EXISTS mqtt_nonces (
    nonce            TEXT PRIMARY KEY,
    device_id        TEXT NOT NULL,
    seen_at          INTEGER NOT NULL,
    expires_at       INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_nonces_expires ON mqtt_nonces(expires_at);

-- ── 行为(EC-BAID 算法源,保留旧设计,字段名 device_id 统一) ───
CREATE TABLE IF NOT EXISTS behavior (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id       TEXT NOT NULL,
    ts              INTEGER NOT NULL,
    win_s           INTEGER NOT NULL DEFAULT 60,
    static_s        INTEGER NOT NULL DEFAULT 0,
    walk_slow_s     INTEGER NOT NULL DEFAULT 0,
    walk_fast_s     INTEGER NOT NULL DEFAULT 0,
    run_s           INTEGER NOT NULL DEFAULT 0,
    shake_or_fall_s INTEGER NOT NULL DEFAULT 0,
    shake_n         INTEGER NOT NULL DEFAULT 0,
    intensity_avg   REAL    NOT NULL DEFAULT 0,
    steps           INTEGER,
    received_at     INTEGER NOT NULL,
    UNIQUE(device_id, ts)
);
CREATE INDEX IF NOT EXISTS idx_behavior_dev_ts ON behavior(device_id, ts DESC);
"""


def _now() -> int:
    return int(time.time())


async def init() -> None:
    global _db
    config.DATA_DIR.mkdir(parents=True, exist_ok=True)
    _db = await aiosqlite.connect(str(config.DB_PATH))
    _db.row_factory = aiosqlite.Row
    await _db.executescript(SCHEMA)
    await _migrate_batches_add_deleted_at()
    await _migrate_admins_drop_display_name()
    await _migrate_recipients_delivery_fields()
    await _db.commit()
    await _seed_admin()
    log.info("DB ready at %s", config.DB_PATH)


async def close() -> None:
    global _db
    if _db:
        await _db.close()
        _db = None


def conn() -> aiosqlite.Connection:
    assert _db is not None, "DB not initialized"
    return _db


async def _seed_admin() -> None:
    if not config.ADMIN_USERNAME or not config.ADMIN_PASSWORD_HASH:
        return
    cur = await _db.execute("SELECT 1 FROM admins WHERE username=?", (config.ADMIN_USERNAME,))
    if await cur.fetchone():
        return
    await _db.execute(
        "INSERT INTO admins(username, password_hash, role, enabled, created_at) "
        "VALUES(?,?,?,1,?)",
        (config.ADMIN_USERNAME, config.ADMIN_PASSWORD_HASH, "admin", _now()),
    )
    await _db.commit()
    log.info("seeded admin user: %s", config.ADMIN_USERNAME)


async def _migrate_admins_drop_display_name() -> None:
    cur = await _db.execute("PRAGMA table_info(admins)")
    columns = [dict(row)["name"] for row in await cur.fetchall()]
    if "display_name" not in columns:
        return
    await _db.executescript(
        """
        CREATE TABLE admins_new (
            username         TEXT PRIMARY KEY,
            password_hash    TEXT NOT NULL,
            role             TEXT NOT NULL DEFAULT 'operator',
            enabled          INTEGER NOT NULL DEFAULT 1,
            created_at       INTEGER NOT NULL,
            last_login_at    INTEGER
        );
        INSERT INTO admins_new(username, password_hash, role, enabled, created_at, last_login_at)
        SELECT username, password_hash, role, enabled, created_at, last_login_at FROM admins;
        DROP TABLE admins;
        ALTER TABLE admins_new RENAME TO admins;
        """
    )


async def _migrate_batches_add_deleted_at() -> None:
    cur = await _db.execute("PRAGMA table_info(batches)")
    columns = [dict(row)["name"] for row in await cur.fetchall()]
    if "deleted_at" not in columns:
        await _db.execute("ALTER TABLE batches ADD COLUMN deleted_at INTEGER")
    await _db.execute("CREATE INDEX IF NOT EXISTS idx_batches_deleted ON batches(deleted_at)")


async def _migrate_recipients_delivery_fields() -> None:
    cur = await _db.execute("PRAGMA table_info(message_recipients)")
    columns = [dict(row)["name"] for row in await cur.fetchall()]
    if "attempt_count" not in columns:
        await _db.execute("ALTER TABLE message_recipients ADD COLUMN attempt_count INTEGER NOT NULL DEFAULT 0")
        await _db.execute(
            "UPDATE message_recipients SET attempt_count=COALESCE(("
            "SELECT attempt_count FROM messages WHERE messages.msg_id=message_recipients.msg_id"
            "), 0)"
        )
    if "next_retry_at" not in columns:
        await _db.execute("ALTER TABLE message_recipients ADD COLUMN next_retry_at INTEGER")
        await _db.execute(
            "UPDATE message_recipients SET next_retry_at=("
            "SELECT next_retry_at FROM messages WHERE messages.msg_id=message_recipients.msg_id"
            ") WHERE status IN ('queued','sent')"
        )
    if "last_send_at" not in columns:
        await _db.execute("ALTER TABLE message_recipients ADD COLUMN last_send_at INTEGER")
        await _db.execute(
            "UPDATE message_recipients SET last_send_at=("
            "SELECT last_send_at FROM messages WHERE messages.msg_id=message_recipients.msg_id"
            ") WHERE status IN ('sent','delivered','displayed','acknowledged')"
        )
    await _db.execute("CREATE INDEX IF NOT EXISTS idx_recipients_retry ON message_recipients(status, next_retry_at)")


# ── 通用 helper ─────────────────────────────────────────
async def fetchone(sql: str, params: tuple = ()) -> dict | None:
    cur = await conn().execute(sql, params)
    row = await cur.fetchone()
    return dict(row) if row else None


async def fetchall(sql: str, params: tuple = ()) -> list[dict]:
    cur = await conn().execute(sql, params)
    return [dict(r) for r in await cur.fetchall()]


async def execute(sql: str, params: tuple = ()) -> int:
    async with _write_lock:
        cur = await conn().execute(sql, params)
        await conn().commit()
        return cur.lastrowid or 0


@asynccontextmanager
async def begin():
    """原子多语句写块。持有写锁期间直接操作底层连接，成功后统一 commit，异常则 rollback。"""
    async with _write_lock:
        try:
            yield _db
            await _db.commit()
        except BaseException:
            await _db.rollback()
            raise


# ── 维护 ────────────────────────────────────────────────
async def cleanup_expired_nonces() -> int:
    async with _write_lock:
        now = _now()
        cur = await conn().execute("DELETE FROM mqtt_nonces WHERE expires_at<?", (now,))
        await conn().commit()
        return cur.rowcount or 0
