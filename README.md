# Esp32Demos

Quick entrypoint for local development of ESP32 WebRTC + gateway demos.

## Repo layout

- `IDF_esp32p4_webrtc/`: ESP-IDF firmware project (ESP32 doorbell/WebRTC app).
- `webrtc_public_gateway/`: Janus + Caddy + Mosquitto docker stack and web viewer.
- `submodules/robot_canbus/`: CAN bus helper module used by firmware.


## 1) One-command config bootstrap

To generate Janus secrets and sync room settings into gateway + firmware sdkconfig:

```bash
cd webrtc_public_gateway
./scripts/bootstrap_janus_and_sdkconfig.sh --room-id 1234 --signal-url http://192.168.19.25:8080/janus --mqtt-broker-uri mqtt://192.168.19.25:1883
```

## 2) Firmware quickstart

```bash
source "$IDF_PATH/export.sh"
cd IDF_esp32p4_webrtc
idf.py build flash monitor
```

If serial port is not auto-detected:

```bash
idf.py -p /dev/ttyACM0 build flash monitor
```

## 3) Gateway quickstart (local)

```bash
cd webrtc_public_gateway
docker compose up -d --force-recreate
```

Open:

- `http://<your-host-ip>:8080/`

## 4) Detailed docs

- Gateway details: `webrtc_public_gateway/README.md`
- ESP32 project config/options: `IDF_esp32p4_webrtc/main/Kconfig`
