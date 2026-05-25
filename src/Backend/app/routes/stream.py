"""SSE 实时流。"""
from __future__ import annotations
from fastapi import APIRouter, Depends
from fastapi.responses import StreamingResponse

from .. import auth as A
from .. import events

router = APIRouter()


@router.get("")
async def stream(admin: dict = Depends(A.current_admin)):
    return StreamingResponse(events.stream(), media_type="text/event-stream",
                             headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})
