#!/usr/bin/env python3
import argparse
import struct
import time

import cv2
import numpy as np
from websocket import create_connection
from websocket import WebSocketTimeoutException

MAGIC = 0x5753494D
VERSION = 1
VERSION_NEW = 2
VERSION_V3 = 3
HEADER_FMT_V2 = "<IHHIHHIHHI"
HEADER_FMT_V3 = "<IHHIHHI"
POINT_FMT = "<HHI"
POINT_SIZE = struct.calcsize(POINT_FMT)


def draw_overlay(img, frame_id, points, age_ms):
    for x, y, score in points:
        cv2.circle(img, (x, y), 4, (0, 255, 0), 1, cv2.LINE_AA)
        # cv2.putText(img, str(score), (x + 4, y - 4), cv2.FONT_HERSHEY_SIMPLEX, 0.35, (0, 255, 255), 1, cv2.LINE_AA)

    status = f"frame={frame_id} selected={len(points)} age_ms={age_ms}"
    cv2.putText(img, status, (12, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 0), 1, cv2.LINE_AA)


def parse_packet(payload):
    if len(payload) < 8:
        return None

    magic, version = struct.unpack_from("<IH", payload, 0)
    if magic != MAGIC:
        return None

    if version == VERSION_V3:
        header_size = struct.calcsize(HEADER_FMT_V3)
        if len(payload) < header_size:
            return None
        _, _, point_count, frame_id, small_w, small_h, small_size = struct.unpack_from(HEADER_FMT_V3, payload, 0)
        points_bytes = point_count * POINT_SIZE
        expected = header_size + points_bytes + small_size
        if len(payload) < expected:
            return None
        points = []
        offset = header_size
        for _ in range(point_count):
            x, y, score = struct.unpack_from(POINT_FMT, payload, offset)
            points.append((x, y, score))
            offset += POINT_SIZE
        small_raw = payload[offset:offset + small_size]
        if small_size != small_w * small_h or small_w <= 0 or small_h <= 0:
            return None
        gray = np.frombuffer(small_raw, dtype=np.uint8).reshape((small_h, small_w))
        img_small = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)
        return frame_id, points, None, img_small

    # Backward compatibility for older packet format.
    if version not in (VERSION, VERSION_NEW):
        return None
    header_size = struct.calcsize(HEADER_FMT_V2)
    if len(payload) < header_size:
        return None
    _, _, point_count, frame_id, full_w, full_h, jpeg_size, small_w, small_h, small_size = struct.unpack_from(HEADER_FMT_V2, payload, 0)
    points_bytes = point_count * POINT_SIZE
    expected = header_size + points_bytes + jpeg_size + small_size
    if len(payload) < expected:
        return None
    points = []
    offset = header_size
    for _ in range(point_count):
        x, y, score = struct.unpack_from(POINT_FMT, payload, offset)
        points.append((x, y, score))
        offset += POINT_SIZE
    jpg = payload[offset:offset + jpeg_size]
    offset += jpeg_size
    small_raw = payload[offset:offset + small_size]
    img = cv2.imdecode(np.frombuffer(jpg, dtype=np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        return None
    if img.shape[1] != full_w or img.shape[0] != full_h:
        img = cv2.resize(img, (full_w, full_h), interpolation=cv2.INTER_LINEAR)
    img_small = None
    if small_size == small_w * small_h and small_w > 0 and small_h > 0:
        gray = np.frombuffer(small_raw, dtype=np.uint8).reshape((small_h, small_w))
        img_small = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)
    return frame_id, points, img, img_small


def main():
    parser = argparse.ArgumentParser(description="ESP32P4 WebSocket pixel-selector debug viewer")
    parser.add_argument("--ws-url", required=True, help="ws://<board-ip>:8080/ws")
    parser.add_argument("--timeout-sec", type=float, default=30.0, help="WebSocket recv timeout in seconds")
    args = parser.parse_args()

    ws = create_connection(args.ws_url, timeout=5)
    ws.settimeout(args.timeout_sec)

    try:
        while True:
            t0 = time.time()
            try:
                payload = ws.recv()
            except WebSocketTimeoutException:
                continue
            if not isinstance(payload, (bytes, bytearray)):
                continue

            parsed = parse_packet(payload)
            if not parsed:
                continue

            frame_id, points, img, img_small = parsed
            age_ms = int((time.time() - t0) * 1000)
            if img_small is not None:
                draw_overlay(img_small, frame_id, points, age_ms)
                cv2.imshow("WS PixelSelector Quarter", img_small)
            if img is not None:
                points_full = [(x * 2, y * 2, s) for (x, y, s) in points]
                draw_overlay(img, frame_id, points_full, age_ms)
                cv2.imshow("WS Full JPEG", img)
            if (cv2.waitKey(1) & 0xFF) == ord("q"):
                break
    finally:
        ws.close()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
