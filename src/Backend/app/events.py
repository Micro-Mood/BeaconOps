"""SSE 事件总线 — 把消息/设备的状态变化推给所有连接的前端。

事件 schema 见 data-model-v1.md §3。
"""
from __future__ import annotations
import asyncio
import json
import logging
from typing import AsyncIterator

log = logging.getLogger("beacon.events")

_subscribers: set[asyncio.Queue] = set()


def emit(event_type: str, **fields) -> None:
    """非阻塞,把事件丢给所有订阅者。"""
    fields["type"] = event_type
    payload = json.dumps(fields, ensure_ascii=False)
    for q in _subscribers:
        try:
            q.put_nowait(payload)
        except asyncio.QueueFull:
            log.warning("SSE subscriber lagging, event dropped: %s", event_type)


async def stream() -> AsyncIterator[str]:
    """SSE 生成器。"""
    q: asyncio.Queue = asyncio.Queue(maxsize=200)
    _subscribers.add(q)
    try:
        # 立即发个 hello 让前端确认连上
        yield "event: hello\ndata: {}\n\n"
        while True:
            try:
                payload = await asyncio.wait_for(q.get(), timeout=15)
                yield f"data: {payload}\n\n"
            except asyncio.TimeoutError:
                # 心跳保活
                yield ": ping\n\n"
    finally:
        _subscribers.discard(q)
