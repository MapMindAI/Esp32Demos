# MQTT-Controlled WebRTC Flow

This document describes the current control logic where MQTT controls WebRTC lifecycle on ESP32.

## Goal

- Keep MQTT running independently from WebRTC.
- Start WebRTC only on explicit client request.
- Stop WebRTC automatically when client heartbeat disappears (power saving).

## Topics

Using `ROOM_ID` as prefix:

- Command topic: `ROOM_ID/command`
- Status topic: `ROOM_ID/status`
- Memory topic: `ROOM_ID/memory`

## Command Protocol

Commands sent to `ROOM_ID/command`:

- `OPEN_WEBRTC`: request ESP32 to start WebRTC.
- `CLOSE_WEBRTC`: request ESP32 to stop WebRTC.
- `HEARTBEAT`: client liveness signal.

## ESP32 Logic

### MQTT lifecycle

- MQTT starts when network is connected.
- MQTT stops when network is disconnected.
- MQTT does **not** depend on WebRTC state.

### WebRTC lifecycle

- On `OPEN_WEBRTC`:
  - if WebRTC not running, call `start_webrtc(NULL)`.
- On `CLOSE_WEBRTC`:
  - call `stop_webrtc()`.
- On `HEARTBEAT`:
  - update last heartbeat timestamp.

### Heartbeat timeout

- If WebRTC is running and no heartbeat is received for `WEBRTC_HEARTBEAT_TIMEOUT_MS` (currently 15s):
  - stop WebRTC.
  - publish status: `{"webrtc":"stopped","reason":"heartbeat_timeout"}`.

### Status publish

ESP32 publishes status on `ROOM_ID/status`:

- `{"webrtc":"idle"}` after MQTT control initializes.
- `{"webrtc":"ready"}` when WebRTC connected.
- `{"webrtc":"stopped"}` when WebRTC disconnects/stops.

## Web Client Logic

Expected sequence:

1. Connect MQTT.
2. Send `OPEN_WEBRTC`.
3. Start sending `HEARTBEAT` every 3s.
4. Wait for `ROOM_ID/status` with `{"webrtc":"ready"}`.
5. Connect Janus/WebRTC viewer.

On client disconnect:

- Send `CLOSE_WEBRTC`.
- Stop heartbeat timer.

## Key Files

- ESP32 MQTT bridge:
  - `IDF_esp32p4_webrtc/main/mqtt_bridge.c`
  - `IDF_esp32p4_webrtc/main/mqtt_bridge.h`
- ESP32 control loop:
  - `IDF_esp32p4_webrtc/main/main.c`
- ESP32 WebRTC state helpers:
  - `IDF_esp32p4_webrtc/main/webrtc.c`
  - `IDF_esp32p4_webrtc/main/common.h`
- Web client orchestration:
  - `webrtc_public_gateway/web/app.js`
