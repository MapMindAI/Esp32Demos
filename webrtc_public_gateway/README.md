# Public WebRTC Gateway for ESP32 Doorbell

This subproject deploys a secure, public-facing WebRTC stack on Linux:

- `Janus Gateway` as signaling/SFU (ESP32 publishes, browser subscribes)
- `Caddy` as HTTPS/WSS edge with automatic TLS
- `Web client` for remote viewing from Internet

## 1. Architecture

1. ESP32 connects outbound to Janus (`https://<domain>/janus`) using built-in Janus signaling in `esp_webrtc`.
2. Browser opens `https://<domain>/` and connects to Janus via WSS (`/janus-ws`).
3. Janus relays media to browser over DTLS-SRTP.

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

```bash
sudo ufw allow 8080/tcp
sudo ufw allow 8443/tcp
sudo ufw allow 10000:10100/udp
sudo ufw status
```

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

Notes:
- `nat_1_1_mapping` is intentionally disabled in `janus.jcfg` for local testing.
- For cloud/public Internet testing, re-enable `nat_1_1_mapping` with the real public IP and use HTTPS/WSS via your domain.

## Steps

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

3. Edit `.env`:

- `PUBLIC_FQDN=your-domain`

4. Start stack:

```bash
docker compose up -d
```

5. Open viewer:

- `https://<PUBLIC_FQDN>/`

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

## 5.1 Enable token + API secret checks

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

## 6. Operational notes

- If browser connects but no stream appears, verify:
  - ESP32 is online and joined room
  - Room ID/PIN match on both sides
  - UDP `10000-10100` open both cloud SG and host firewall
  - `nat_1_1_mapping` points to correct public IP
- For production, rotate all secrets and restrict admin API access to localhost/VPN only.
