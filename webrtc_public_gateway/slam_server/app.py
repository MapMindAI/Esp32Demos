from __future__ import annotations

import base64
import os
import time
from typing import Any, Dict, Optional

import cv2
import numpy as np
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

from orbslam_engine import EngineConfig, OrbSlamEngine


def _env_int(name: str, default: int) -> int:
    raw = os.getenv(name, str(default)).strip()
    try:
        return int(raw)
    except Exception:
        return default


def _create_engine() -> OrbSlamEngine:
    cfg = EngineConfig(
        vocab_path=os.getenv("SLAM_VOCAB_PATH", "/opt/orbslam/Vocabulary/ORBvoc.txt"),
        settings_path=os.getenv("SLAM_SETTINGS_PATH", "/opt/orbslam/config/monocular.yaml"),
        sensor=os.getenv("SLAM_SENSOR", "MONOCULAR"),
        max_trajectory_points=_env_int("SLAM_MAX_TRAJECTORY_POINTS", 3000),
        map_sample_stride=max(1, _env_int("SLAM_MAP_SAMPLE_STRIDE", 4)),
        max_map_points=max(100, _env_int("SLAM_MAX_MAP_POINTS", 4000)),
    )
    return OrbSlamEngine(cfg)


engine = _create_engine()

app = FastAPI(title="ORB-SLAM3 Bridge", version="0.1.0")


class FrameRequest(BaseModel):
    image_base64: str = Field(..., description="JPEG/PNG data URL or plain base64")
    timestamp_ms: Optional[float] = Field(default=None)


def _decode_image_bgr(image_base64: str) -> np.ndarray:
    payload = image_base64.strip()
    if "," in payload:
        payload = payload.split(",", 1)[1]
    try:
        raw = base64.b64decode(payload, validate=True)
    except Exception as exc:
        raise HTTPException(status_code=400, detail=f"Invalid base64 payload: {exc}") from exc

    img_arr = np.frombuffer(raw, dtype=np.uint8)
    frame = cv2.imdecode(img_arr, cv2.IMREAD_COLOR)
    if frame is None:
        raise HTTPException(status_code=400, detail="Failed to decode image bytes")
    return frame


def _build_response(snapshot: Dict[str, Any]) -> Dict[str, Any]:
    return {"ok": True, "timestamp_ms": time.time() * 1000.0, "slam": snapshot}


@app.get("/slam/health")
def health() -> Dict[str, Any]:
    return {"ok": True}


@app.get("/slam/api/state")
def state() -> Dict[str, Any]:
    return _build_response(engine.snapshot())


@app.post("/slam/api/control/start")
def start() -> Dict[str, Any]:
    return _build_response(engine.start())


@app.post("/slam/api/control/stop")
def stop() -> Dict[str, Any]:
    return _build_response(engine.stop())


@app.post("/slam/api/control/reset")
def reset() -> Dict[str, Any]:
    return _build_response(engine.reset())


@app.post("/slam/api/frame")
def frame(req: FrameRequest) -> Dict[str, Any]:
    frame_bgr = _decode_image_bgr(req.image_base64)
    timestamp_ms = float(req.timestamp_ms) if req.timestamp_ms is not None else time.time() * 1000.0
    snapshot = engine.process_frame(frame_bgr, timestamp_ms)
    return _build_response(snapshot)

