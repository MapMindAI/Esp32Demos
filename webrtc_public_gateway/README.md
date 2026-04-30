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
  - run `curl -4 https://api.ipify.org` to get public IP.
- Docker + Docker Compose plugin
- Firewall open:
  - TCP `80`, `443`
  - UDP `10000-10100`

## Steps

1. Copy env template:

```bash
cd webrtc_public_gateway
cp .env.example .env
```

2. Edit `janus/*.jcfg` secrets/placeholders:

- `janus.jcfg`:
  - `admin_secret`
  - `nat_1_1_mapping` to your public IP
- `janus.plugin.videoroom.jcfg`:
  - `pin`, `secret`, `admin_key`
  - `room-1234` (or change room id consistently)

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

## 6. Operational notes

- If browser connects but no stream appears, verify:
  - ESP32 is online and joined room
  - Room ID/PIN match on both sides
  - UDP `10000-10100` open both cloud SG and host firewall
  - `nat_1_1_mapping` points to correct public IP
- For production, rotate all secrets and restrict admin API access to localhost/VPN only.
