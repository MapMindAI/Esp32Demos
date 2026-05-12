# Public WebRTC Gateway for ESP32 Doorbell

This subproject deploys a secure, public-facing WebRTC stack on Linux:

- `Janus Gateway` as signaling/SFU (ESP32 publishes, browser subscribes)
- `Caddy` as HTTPS/WSS edge with automatic TLS
- `Web client` for remote viewing from Internet
- `slam-server` bridge API for ORB-SLAM3 pose trajectory + sparse map rendering

## 1. Architecture

1. ESP32 connects outbound to Janus (`https://<domain>/janus`) using built-in Janus signaling in `esp_webrtc`.
2. Browser opens `https://<domain>/` and connects to Janus via WSS (`/janus-ws`).
3. Janus relays media to browser over DTLS-SRTP.
4. Browser can optionally push decoded video frames to `/slam/api/frame`; `slam-server` runs ORB-SLAM3 and returns camera 6DOF trajectory and map points.

## 2. Security controls in this setup

- HTTPS/WSS only at edge (Caddy)
- Janus VideoRoom `pin` and `secret`
- Optional Janus token auth support
- Janus admin API bound to loopback only (`127.0.0.1:7088`)
- Tight RTP UDP port range (`10000-10100`) to simplify firewall policy

## 3. Linux deployment

## Prerequisites

- Public Linux host with DNS A record (`webrtc.example.com` -> server IP)
- Docker + Docker Compose plugin
- Firewall open (or change from your cloud server control panel):
  - TCP `8080`, `8443`
  - UDP `10000-10100`

<details>
<summary>ufw cmd in linux</summary>

```bash
sudo ufw allow 8080/tcp
sudo ufw allow 8443/tcp
sudo ufw allow 10000:10100/udp
sudo ufw status
```
</details>

## Quick local functional test (single host)

Use this when you only want to verify Janus/Web viewer basics locally, without public DNS/TLS.

1. Start stack:

```bash
cd webrtc_public_gateway
docker compose up -d
```

2. Open local viewer:

- `http://localhost:8080/`

3. Point ESP32 Janus API base URL to:

- `http://<your-linux-host-ip>:8080/janus`

ARM64 note:

- If your host is ARM64 and you see image platform mismatch warnings, set `JANUS_IMAGE` in `.env` to an ARM64/multi-arch Janus image, then restart compose.
- Example:

```bash
cd webrtc_public_gateway
cp .env.example .env  # if not yet created
sed -i 's|^JANUS_IMAGE=.*|JANUS_IMAGE=sucwangsr/janus-webrtc-gateway-docker:latest|' .env
docker compose up -d --force-recreate
```

Notes:
- `nat_1_1_mapping` is intentionally disabled in `janus.jcfg` for local testing.
- For cloud/public Internet testing, re-enable `nat_1_1_mapping` with the real public IP and use HTTPS/WSS via your domain.

## Server Deployment Steps

or simply :

```
./scripts/bootstrap_janus_and_sdkconfig.sh --room-id 1234 --signal-url http://192.168.19.25:8080/janus --mqtt-broker-uri mqtt://192.168.19.25:1883
```

1. Copy env template:

```bash
cd webrtc_public_gateway
cp .env.example .env
```

2. Edit `janus/*.jcfg` secrets/placeholders:

- `janus.jcfg`:
  - `admin_secret` with `openssl rand -base64 48`
  - `nat_1_1_mapping` to your public IP
- `janus.plugin.videoroom.jcfg`:
  - `pin`, `secret`, `admin_key`
  - `room-1234` (or change room id consistently)
  - room pin : `openssl rand -base64 24 | tr -d '\n'`

3. Edit `.env` setup `PUBLIC_FQDN=your-domain`

4. Start stack:

```bash
docker compose up -d
```

5. Open viewer: `https://<PUBLIC_FQDN>/`

## 4. ESP32 firmware configuration

In `IDF_esp32p4_webrtc`:

1. Run menuconfig and set signaling mode to Janus:

```bash
idf.py menuconfig
```

Path:
- `DoorBell Local Demo -> Signaling backend -> Remote Janus VideoRoom signaling`

2. Configure Janus settings in menuconfig:

- `Janus API base URL`: `https://<PUBLIC_FQDN>/janus`
- `Janus VideoRoom ID`: e.g. `1234`
- `Janus room PIN`: same as `janus.plugin.videoroom.jcfg`
- Optional token/api_secret if enabled server-side

3. Build and flash.

## 5. Optional token auth

If `token_auth = true` is enabled in Janus:

- Create viewer/publisher tokens via admin API.
- Example helper:

```bash
./scripts/add_token.sh <ADMIN_SECRET> <TOKEN> http://127.0.0.1:7088/admin
```

Then paste token in web client `Token` input.

<details>
<summary>Enable token + API secret checks</summary>

1. Edit `janus/janus.jcfg`:

- Set `token_auth = true`
- Set `api_secret = "<your-strong-secret>"`

Example secret:

```bash
openssl rand -base64 32 | tr -d '\n'
```

2. Restart Janus:

```bash
docker compose restart janus
```

3. Create token(s) via admin API:

```bash
./scripts/add_token.sh ${ADMIN_SECRET} ${TOKEN_FOR_DEVICE_OR_VIEWER} http://127.0.0.1:7088/admin
```

4. Configure clients:

- ESP32:
  - `CONFIG_DOORBELL_JANUS_TOKEN="<TOKEN_FOR_DEVICE_OR_VIEWER>"`
  - `CONFIG_DOORBELL_JANUS_API_SECRET="<your-strong-secret>"`
- Web viewer:
  - `Janus Token`: same token (or another valid token you added)
  - `Janus API Secret`: same `api_secret` value from `janus.jcfg`

</details>

## 6. Operational notes

- If browser connects but no stream appears, verify:
  - ESP32 is online and joined room
  - Room ID/PIN match on both sides
  - UDP `10000-10100` open both cloud SG and host firewall
  - `nat_1_1_mapping` points to correct public IP
- For production, rotate all secrets and restrict admin API access to localhost/VPN only.

## 7. MQTT command + telemetry

This project also supports MQTT for bidirectional control/status:

- ESP32 publishes memory telemetry.
- Browser subscribes telemetry and sends command strings.

### Broker service

`docker-compose.yml` includes a Mosquitto broker:

- MQTT TCP: `1883`
- MQTT WebSocket: `9001`

Start/restart:

```bash
cd webrtc_public_gateway
docker compose up -d --force-recreate mosquitto
docker logs mqtt-broker --tail 120
```

### Authentication model

MQTT auth is enabled (`allow_anonymous false`).

- MQTT username: `ROOM_ID`
- MQTT password: `ROOM_PIN`

Broker user/password is initialized at container startup using `.env` values:

- `JANUS_ROOM_ID`
- `JANUS_ROOM_PIN`

### Topic convention

- Memory topic: `<ROOM_ID>/memory`
- Command topic: `<ROOM_ID>/command`

Example with room `1234`:

- `1234/memory`
- `1234/command`

### ESP32 side

Firmware uses:

- `CONFIG_DOORBELL_MQTT_BROKER_URI` for broker URI (menuconfig)
- `CONFIG_DOORBELL_JANUS_ROOM_ID` for MQTT username/topic prefix
- `CONFIG_DOORBELL_JANUS_ROOM_PIN` for MQTT password

Expected log when command arrives:

- `MQTT command received: <your_command>`

### Web UI side

In `http://<host>:8080/`:

- MQTT fields auto-fill from Room ID/PIN.
- Default topics auto-fill as `ROOM_ID/memory` and `ROOM_ID/command`.
- Click `MQTT Connect` to start receiving telemetry.
- Use `Send MQTT Cmd` to send string command to ESP32.

## 8. ORB-SLAM3 Pose + Map in Web UI

The web page now includes a `Visual SLAM` panel with:

- `SLAM Start/Stop/Reset` controls
- live SLAM state (`initializing`, `tracking`, `lost`, `error`)
- current 6DOF pose readout
- 2D map canvas rendering trajectory + sparse map points

### 8.1 Prepare ORB-SLAM3 data files

Create this directory structure under `webrtc_public_gateway/`:

```bash
mkdir -p slam_data/Vocabulary slam_data/config
```

Place files:

- `slam_data/Vocabulary/ORBvoc.txt`
- `slam_data/config/monocular.yaml`

`monocular.yaml` must match your ESP32 camera intrinsics/resolution.

### 8.2 Configure env (optional)

Defaults are already in `.env.example`:

- `SLAM_DATA_DIR=./slam_data`
- `SLAM_VOCAB_PATH=/opt/orbslam/Vocabulary/ORBvoc.txt`
- `SLAM_SETTINGS_PATH=/opt/orbslam/config/monocular.yaml`

If your filenames differ, adjust those variables in `.env`.

### 8.3 Start services

```bash
cd webrtc_public_gateway
docker compose up -d --build slam-server caddy janus mosquitto
```

### 8.4 Use in browser

1. Open the web app (`http://localhost:8080/` for local testing).
2. Connect WebRTC as usual.
3. In `Visual SLAM`, click `SLAM Start`.
4. Move camera to generate parallax; map/trajectory update live.

Notes:

- Monocular ORB-SLAM3 trajectory scale is relative (not metric unless additional scale reference exists).
- If the SLAM status shows file/module errors, check:
  - `slam-server` container logs
  - vocabulary/settings paths
  - whether `orbslam3-python` is installed for your host architecture
