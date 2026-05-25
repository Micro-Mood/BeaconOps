"""Broker control helpers for security-sensitive admin actions."""
from __future__ import annotations

import asyncio
import logging

log = logging.getLogger("beacon.broker")


async def restart_mosquitto(reason: str) -> None:
    """Restart Mosquitto to tear down existing MQTT sessions immediately."""
    try:
        proc = await asyncio.create_subprocess_exec(
            "systemctl", "restart", "mosquitto",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
    except FileNotFoundError:
        log.warning("mosquitto restart skipped: systemctl unavailable reason=%s", reason)
        return
    stdout, stderr = await proc.communicate()
    output = (stdout or b"").decode("utf-8", errors="replace") + (stderr or b"").decode("utf-8", errors="replace")
    if proc.returncode == 0:
        log.info("mosquitto restarted: %s", reason)
        return
    log.warning("mosquitto restart failed: %s output=%s", reason, output.strip())