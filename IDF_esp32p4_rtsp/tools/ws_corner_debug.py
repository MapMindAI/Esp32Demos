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
HEADER_FMT = "<IHHIHHI"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
POINT_FMT = "<HHI"
POINT_SIZE = struct.calcsize(POINT_FMT)


def draw_overlay(img, frame_id, points, age_ms):
    for x, y, score in points:
        cv2.circle(img, (x, y), 4, (0, 255, 0), 1, cv2.LINE_AA)
        cv2.putText(img, str(score), (x + 4, y - 4), cv2.FONT_HERSHEY_SIMPLEX, 0.35, (0, 255, 255), 1, cv2.LINE_AA)

    status = f"frame={frame_id} corners={len(points)} age_ms={age_ms}"
    cv2.putText(img, status, (12, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2, cv2.LINE_AA)


def parse_packet(payload):
    if len(payload) < HEADER_SIZE:
        return None

    magic, version, point_count, frame_id, width, height, jpeg_size = struct.unpack_from(HEADER_FMT, payload, 0)
    if magic != MAGIC or version != VERSION:
        return None

    points_bytes = point_count * POINT_SIZE
    expected = HEADER_SIZE + points_bytes + jpeg_size
    if len(payload) < expected:
        return None

    points = []
    offset = HEADER_SIZE
    for _ in range(point_count):
        x, y, score = struct.unpack_from(POINT_FMT, payload, offset)
        points.append((x, y, score))
        offset += POINT_SIZE

    jpg = payload[offset:offset + jpeg_size]
    img = cv2.imdecode(np.frombuffer(jpg, dtype=np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        return None

    if img.shape[1] != width or img.shape[0] != height:
        img = cv2.resize(img, (width, height), interpolation=cv2.INTER_LINEAR)

    return frame_id, points, img


def main():
    parser = argparse.ArgumentParser(description="ESP32P4 WebSocket JPEG + corner debug viewer")
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

            frame_id, points, img = parsed
            age_ms = int((time.time() - t0) * 1000)
            draw_overlay(img, frame_id, points, age_ms)
            cv2.imshow("WS Corner Debug", img)
            if (cv2.waitKey(1) & 0xFF) == ord("q"):
                break
    finally:
        ws.close()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
