"""服务器端 MQTT 客户端(gmqtt)。

只暴露:
  - start() / stop() lifecycle
  - publish(topic, payload, qos, retain) -> bool
  - subscribe topic patterns & 路由到 handlers/uplink.py

不做下行重试 — 重试在 messages.py 中按 next_retry_at 调度,
此模块只负责"现在 publish 这一次,成功/失败"。
"""
from __future__ import annotations
import asyncio
import json
import logging
import re
from typing import Awaitable, Callable

from gmqtt import Client as GMQTTClient

from . import config

log = logging.getLogger("beacon.mqtt")

_client: GMQTTClient | None = None
_connected = asyncio.Event()
_uplink_handler: Callable[[str, dict], Awaitable[None]] | None = None

_TOPIC_RE = re.compile(r"^device/([0-9a-f]{12})/(status|uplink/(ack|event|health|profile))$")


def set_uplink_handler(handler: Callable[[str, dict], Awaitable[None]]) -> None:
    """注入处理 {topic, payload_dict} 的 async 函数。"""
    global _uplink_handler
    _uplink_handler = handler


def _on_connect(client, flags, rc, properties):
    log.info("MQTT connected rc=%s", rc)
    client.subscribe("device/+/status",     qos=1)
    client.subscribe("device/+/uplink/ack",     qos=1)
    client.subscribe("device/+/uplink/event",   qos=1)
    client.subscribe("device/+/uplink/health",  qos=0)
    client.subscribe("device/+/uplink/profile", qos=1)
    _connected.set()


def _on_disconnect(client, packet, exc=None):
    log.warning("MQTT disconnected: %s", exc)
    _connected.clear()


async def _on_message(client, topic, payload, qos, properties):
    if _uplink_handler is None:
        return
    if not _TOPIC_RE.match(topic):
        return
    try:
        data = json.loads(payload.decode("utf-8")) if payload else {}
    except Exception:
        log.warning("bad json on %s len=%s", topic, len(payload or b""))
        return
    try:
        await _uplink_handler(topic, data)
    except Exception:
        log.exception("uplink handler error on %s", topic)


async def start() -> None:
    global _client
    _client = GMQTTClient(config.MQTT_CLIENT_ID, clean_session=True)
    if config.MQTT_USERNAME:
        _client.set_auth_credentials(config.MQTT_USERNAME, config.MQTT_PASSWORD or None)
    _client.on_connect    = _on_connect
    _client.on_disconnect = _on_disconnect
    _client.on_message    = _on_message
    log.info("MQTT connecting %s:%s", config.MQTT_HOST, config.MQTT_PORT)
    await _client.connect(config.MQTT_HOST, config.MQTT_PORT, keepalive=60)


async def stop() -> None:
    global _client
    if _client is not None:
        try:
            await _client.disconnect()
        except Exception:
            pass
        _client = None
    _connected.clear()


def is_connected() -> bool:
    return _connected.is_set()


async def publish(topic: str, payload: dict, qos: int = 1, retain: bool = False) -> bool:
    if _client is None or not _connected.is_set():
        return False
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    try:
        _client.publish(topic, body, qos=qos, retain=retain)
        return True
    except Exception as e:
        log.warning("publish err on %s: %s", topic, e)
        return False
