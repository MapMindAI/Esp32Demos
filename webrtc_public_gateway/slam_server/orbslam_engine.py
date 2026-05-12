from __future__ import annotations

import importlib
import os
import threading
import time
from dataclasses import dataclass
from typing import Any, Dict, List, Optional

import numpy as np


def _call_first_callable(obj: Any, names: List[str], *args: Any, **kwargs: Any) -> Any:
    for name in names:
        fn = getattr(obj, name, None)
        if callable(fn):
            return fn(*args, **kwargs)
    raise AttributeError(f"Missing callable in target object. tried={names}")


def _first_existing_attr(obj: Any, names: List[str]) -> Any:
    for name in names:
        if hasattr(obj, name):
            return getattr(obj, name)
    return None


def _as_pose_matrix(raw: Any) -> Optional[np.ndarray]:
    if raw is None:
        return None
    try:
        arr = np.asarray(raw, dtype=np.float64)
    except Exception:
        return None

    if arr.shape == (4, 4):
        return arr
    if arr.shape == (3, 4):
        out = np.eye(4, dtype=np.float64)
        out[:3, :4] = arr
        return out
    if arr.size == 16:
        return arr.reshape((4, 4))
    if arr.size == 12:
        out = np.eye(4, dtype=np.float64)
        out[:3, :4] = arr.reshape((3, 4))
        return out
    return None


def _rotation_to_quaternion(rot: np.ndarray) -> np.ndarray:
    m = rot
    tr = float(m[0, 0] + m[1, 1] + m[2, 2])
    if tr > 0.0:
        s = np.sqrt(tr + 1.0) * 2.0
        qw = 0.25 * s
        qx = (m[2, 1] - m[1, 2]) / s
        qy = (m[0, 2] - m[2, 0]) / s
        qz = (m[1, 0] - m[0, 1]) / s
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2.0
        qw = (m[2, 1] - m[1, 2]) / s
        qx = 0.25 * s
        qy = (m[0, 1] + m[1, 0]) / s
        qz = (m[0, 2] + m[2, 0]) / s
    elif m[1, 1] > m[2, 2]:
        s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2.0
        qw = (m[0, 2] - m[2, 0]) / s
        qx = (m[0, 1] + m[1, 0]) / s
        qy = 0.25 * s
        qz = (m[1, 2] + m[2, 1]) / s
    else:
        s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2.0
        qw = (m[1, 0] - m[0, 1]) / s
        qx = (m[0, 2] + m[2, 0]) / s
        qy = (m[1, 2] + m[2, 1]) / s
        qz = 0.25 * s
    q = np.array([qx, qy, qz, qw], dtype=np.float64)
    norm = np.linalg.norm(q)
    if norm > 0:
        q /= norm
    return q


def _extract_xyz(value: Any) -> Optional[List[float]]:
    if value is None:
        return None

    if isinstance(value, dict):
        if all(k in value for k in ("x", "y", "z")):
            return [float(value["x"]), float(value["y"]), float(value["z"])]

    try:
        arr = np.asarray(value, dtype=np.float64).reshape(-1)
        if arr.size >= 3:
            return [float(arr[0]), float(arr[1]), float(arr[2])]
    except Exception:
        return None
    return None


@dataclass
class EngineConfig:
    vocab_path: str
    settings_path: str
    sensor: str
    max_trajectory_points: int
    map_sample_stride: int
    max_map_points: int


class OrbSlamEngine:
    def __init__(self, cfg: EngineConfig) -> None:
        self.cfg = cfg
        self._lock = threading.RLock()
        self._orb_module = None
        self._system = None
        self._state = "idle"
        self._message = "Not started"
        self._frame_count = 0
        self._tracked_frames = 0
        self._lost_frames = 0
        self._last_timestamp_ms = 0.0
        self._last_update_ms = 0.0
        self._current_pose = None
        self._trajectory: List[Dict[str, Any]] = []
        self._map_points: List[List[float]] = []

    def _resolve_sensor_value(self, module: Any) -> Any:
        sensor_name = str(self.cfg.sensor or "MONOCULAR").upper()
        candidates = []

        sensor_obj = _first_existing_attr(module, ["Sensor", "SENSOR", "sensor"])
        if sensor_obj is not None:
            candidates.extend(
                [
                    getattr(sensor_obj, sensor_name, None),
                    getattr(sensor_obj, "MONOCULAR", None),
                    getattr(sensor_obj, "MONO", None),
                ]
            )
        candidates.extend(
            [
                _first_existing_attr(module, [sensor_name, "MONOCULAR", "MONO"]),
                0,
                "MONOCULAR",
            ]
        )
        for candidate in candidates:
            if candidate is not None:
                return candidate
        return 0

    def _create_system(self) -> Any:
        module = None
        import_errors: List[str] = []
        for mod_name in ("orbslam3", "orbslam3_python"):
            try:
                module = importlib.import_module(mod_name)
                break
            except Exception as exc:
                import_errors.append(f"{mod_name}: {exc}")
        if module is None:
            raise RuntimeError(
                "Failed to import ORB-SLAM3 Python module. "
                f"Tried orbslam3/orbslam3_python. errors={import_errors}"
            )

        self._orb_module = module
        sensor_value = self._resolve_sensor_value(module)
        vocab = self.cfg.vocab_path
        settings = self.cfg.settings_path

        constructors = [
            lambda: module.System(vocab, settings, sensor_value, False),
            lambda: module.System(vocab, settings, sensor_value),
            lambda: module.System(vocab, settings),
            lambda: module.ORBSystem(vocab, settings, sensor_value, False),
            lambda: module.ORBSystem(vocab, settings, sensor_value),
        ]

        errors: List[str] = []
        for build in constructors:
            try:
                system = build()
                init_fn = _first_existing_attr(system, ["initialize", "Initialize"])
                if callable(init_fn):
                    init_fn()
                return system
            except Exception as exc:
                errors.append(str(exc))

        raise RuntimeError(f"Failed to construct ORB-SLAM3 System. errors={errors}")

    def _shutdown_system(self) -> None:
        if self._system is None:
            return
        try:
            _call_first_callable(self._system, ["shutdown", "Shutdown", "close", "Close"])
        except Exception:
            pass
        self._system = None

    def _track_frame(self, frame_bgr: np.ndarray, timestamp_s: float) -> Any:
        return _call_first_callable(
            self._system,
            [
                "TrackMonocular",
                "track_monocular",
                "process_image_mono",
                "process_image_monocular",
                "track_mono",
            ],
            frame_bgr,
            timestamp_s,
        )

    def _read_pose_raw(self) -> Any:
        return _call_first_callable(
            self._system,
            [
                "get_frame_pose",
                "get_current_pose",
                "GetCurrentPose",
                "get_pose",
                "GetPose",
            ],
        )

    def _pose_from_matrix(self, mat_tcw: np.ndarray, timestamp_ms: float) -> Dict[str, Any]:
        rot_cw = mat_tcw[:3, :3]
        t_cw = mat_tcw[:3, 3]

        rot_wc = rot_cw.T
        t_wc = -rot_wc @ t_cw
        quat = _rotation_to_quaternion(rot_wc)

        return {
            "timestamp_ms": float(timestamp_ms),
            "x": float(t_wc[0]),
            "y": float(t_wc[1]),
            "z": float(t_wc[2]),
            "qx": float(quat[0]),
            "qy": float(quat[1]),
            "qz": float(quat[2]),
            "qw": float(quat[3]),
        }

    def _extract_map_points(self) -> List[List[float]]:
        raw_points = None
        methods = [
            "get_current_points",
            "get_map_points",
            "GetMapPoints",
            "get_all_map_points",
            "GetAllMapPoints",
        ]
        for method in methods:
            fn = getattr(self._system, method, None)
            if callable(fn):
                try:
                    raw_points = fn()
                    break
                except Exception:
                    continue

        if raw_points is None:
            return self._map_points

        parsed: List[List[float]] = []
        for item in raw_points:
            xyz = _extract_xyz(item)
            if xyz is None and isinstance(item, (list, tuple)) and len(item) >= 1:
                xyz = _extract_xyz(item[0])
            if xyz is not None:
                parsed.append(xyz)

        if not parsed:
            return self._map_points

        stride = max(1, int(self.cfg.map_sample_stride))
        sampled = parsed[::stride]
        if len(sampled) > self.cfg.max_map_points:
            sampled = sampled[: self.cfg.max_map_points]
        return sampled

    def start(self) -> Dict[str, Any]:
        with self._lock:
            if self._system is not None and self._state in {"initializing", "tracking", "lost"}:
                return self.snapshot()

            if not self.cfg.vocab_path or not self.cfg.settings_path:
                self._state = "error"
                self._message = "Missing SLAM_VOCAB_PATH or SLAM_SETTINGS_PATH"
                return self.snapshot()

            if not os.path.exists(self.cfg.vocab_path):
                self._state = "error"
                self._message = f"Vocabulary not found: {self.cfg.vocab_path}"
                return self.snapshot()
            if not os.path.exists(self.cfg.settings_path):
                self._state = "error"
                self._message = f"Settings file not found: {self.cfg.settings_path}"
                return self.snapshot()

            try:
                self._system = self._create_system()
                self._state = "initializing"
                self._message = "ORB-SLAM3 initialized"
            except Exception as exc:
                self._state = "error"
                self._message = str(exc)
                self._shutdown_system()

            return self.snapshot()

    def stop(self) -> Dict[str, Any]:
        with self._lock:
            self._shutdown_system()
            self._state = "stopped"
            self._message = "Stopped"
            return self.snapshot()

    def reset(self) -> Dict[str, Any]:
        with self._lock:
            if self._system is not None:
                reset_fn = _first_existing_attr(self._system, ["reset", "Reset"])
                if callable(reset_fn):
                    try:
                        reset_fn()
                    except Exception:
                        pass
            self._frame_count = 0
            self._tracked_frames = 0
            self._lost_frames = 0
            self._last_timestamp_ms = 0.0
            self._last_update_ms = 0.0
            self._current_pose = None
            self._trajectory = []
            self._map_points = []
            self._state = "initializing" if self._system is not None else "idle"
            self._message = "Reset completed"
            return self.snapshot()

    def process_frame(self, frame_bgr: np.ndarray, timestamp_ms: float) -> Dict[str, Any]:
        with self._lock:
            if self._system is None:
                self._state = "error"
                self._message = "SLAM engine not started"
                return self.snapshot()

            timestamp_s = float(timestamp_ms) / 1000.0
            self._frame_count += 1
            self._last_timestamp_ms = float(timestamp_ms)
            self._last_update_ms = time.time() * 1000.0

            try:
                pose_raw = self._track_frame(frame_bgr, timestamp_s)
                pose_matrix = _as_pose_matrix(pose_raw)
                if pose_matrix is None:
                    pose_matrix = _as_pose_matrix(self._read_pose_raw())

                if pose_matrix is not None:
                    pose = self._pose_from_matrix(pose_matrix, timestamp_ms)
                    self._current_pose = pose
                    self._tracked_frames += 1
                    self._state = "tracking"
                    self._message = "Tracking"
                    self._trajectory.append(pose)
                    if len(self._trajectory) > self.cfg.max_trajectory_points:
                        self._trajectory = self._trajectory[-self.cfg.max_trajectory_points :]
                else:
                    self._lost_frames += 1
                    self._state = "lost"
                    self._message = "Pose unavailable for current frame"

                self._map_points = self._extract_map_points()
            except Exception as exc:
                self._state = "error"
                self._message = str(exc)

            return self.snapshot()

    def snapshot(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "state": self._state,
                "message": self._message,
                "is_running": self._system is not None,
                "frame_count": self._frame_count,
                "tracked_frames": self._tracked_frames,
                "lost_frames": self._lost_frames,
                "last_timestamp_ms": self._last_timestamp_ms,
                "last_update_ms": self._last_update_ms,
                "current_pose": self._current_pose,
                "trajectory": list(self._trajectory),
                "map_points": list(self._map_points),
                "trajectory_size": len(self._trajectory),
                "map_points_size": len(self._map_points),
                "sensor": self.cfg.sensor,
            }

