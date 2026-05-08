# Esp32Demos

Quick entrypoint for local development of ESP32 WebRTC, MQTT gateway, and CAN/servo control demos.

https://github.com/user-attachments/assets/2f8988e1-435c-417a-9031-e2dc0ea89c77

## Repo layout

- `IDF_esp32p4_webrtc/`: ESP32-P4 WebRTC publisher firmware (camera/audio + MQTT command bridge).
- `IDF_esp32c3_servo_control/`: ESP32-C3 CAN receiver + dual-servo controller.
- `webrtc_public_gateway/`: Janus + Caddy + Mosquitto docker stack and web control page.
- `submodules/robot_canbus/`: shared CAN bus helper module.


## 1) One-command config bootstrap

Generate Janus secrets and sync room settings into gateway + firmware `sdkconfig`:

```bash
cd webrtc_public_gateway
./scripts/bootstrap_janus_and_sdkconfig.sh --room-id 1234 --signal-url http://192.168.19.25:8080/janus --mqtt-broker-uri mqtt://192.168.19.25:1883
```

## 2) Start gateway stack

```bash
cd webrtc_public_gateway
docker compose up -d --force-recreate
```

Open:

- `http://<your-host-ip>:8080/`

## 3) Flash ESP32-P4 WebRTC firmware

```bash
source "$IDF_PATH/export.sh"
cd IDF_esp32p4_webrtc
idf.py build flash monitor
```

If serial port is not auto-detected:

```bash
idf.py -p /dev/ttyACM0 build flash monitor
```

## 4) Flash ESP32-C3 servo controller (optional)

```bash
source "$IDF_PATH/export.sh"
cd IDF_esp32c3_servo_control
idf.py build flash monitor
```

Pin mapping is in:

- `IDF_esp32c3_servo_control/main/app_config.h`

## 5) Runtime flow (current)

1. Open web page and connect MQTT.
2. Web sends `OPEN_WEBRTC` + heartbeat.
3. ESP32-P4 starts WebRTC and publishes status.
4. Web attaches to Janus when status reports ready.
5. Robot/Servo buttons send MQTT commands (`ROBOT_*`, `SERVO_*`) continuously while pressed.
6. P4 forwards control commands to CAN.
7. C3 receives CAN and drives servos.

## 6) Detailed docs

- [Gateway details](webrtc_public_gateway/README.md)
- [MQTT-Controlled WebRTC Flow](webrtc_public_gateway/MQTT_WEBRTC_CONTROL_FLOW.md)
- ESP32-P4 project config/options: `IDF_esp32p4_webrtc/main/Kconfig`
- ESP32-C3 app config: `IDF_esp32c3_servo_control/main/app_config.h`
