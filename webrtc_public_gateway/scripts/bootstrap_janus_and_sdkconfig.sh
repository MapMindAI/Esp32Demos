#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATEWAY_DIR="${ROOT_DIR}/webrtc_public_gateway"
JANUS_CFG="${GATEWAY_DIR}/janus/janus.jcfg"
VIDEOROOM_CFG="${GATEWAY_DIR}/janus/janus.plugin.videoroom.jcfg"
ENV_FILE="${GATEWAY_DIR}/.env"
SDKCONFIG="${ROOT_DIR}/IDF_esp32p4_webrtc/sdkconfig"

ROOM_ID="${ROOM_ID:-1234}"
SIGNAL_URL="${SIGNAL_URL:-http://192.168.19.25:8080/janus}"
DISPLAY_NAME="${DISPLAY_NAME:-esp32-doorbell}"
MQTT_BROKER_URI="${MQTT_BROKER_URI:-}"
ENABLE_BACKUP="${ENABLE_BACKUP:-0}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [--room-id N] [--signal-url URL] [--display NAME] [--mqtt-broker-uri URI] [--backup]

Generates and applies:
  - Janus admin_secret            -> janus.jcfg
  - Janus room pin + room secret  -> janus.plugin.videoroom.jcfg
  - Room/pin in .env              -> webrtc_public_gateway/.env
  - Janus fields in sdkconfig     -> IDF_esp32p4_webrtc/sdkconfig

Optional env vars:
  ROOM_ID, SIGNAL_URL, DISPLAY_NAME, MQTT_BROKER_URI, ENABLE_BACKUP
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --room-id)
      ROOM_ID="$2"
      shift 2
      ;;
    --signal-url)
      SIGNAL_URL="$2"
      shift 2
      ;;
    --display)
      DISPLAY_NAME="$2"
      shift 2
      ;;
    --mqtt-broker-uri)
      MQTT_BROKER_URI="$2"
      shift 2
      ;;
    --backup)
      ENABLE_BACKUP=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "${MQTT_BROKER_URI}" ]]; then
  # Derive mqtt://<host>:1883 from SIGNAL_URL.
  _host="$(echo "${SIGNAL_URL}" | sed -E 's#^[a-zA-Z]+://([^/:]+).*$#\1#')"
  MQTT_BROKER_URI="mqtt://${_host}:1883"
fi

for f in "$JANUS_CFG" "$VIDEOROOM_CFG" "$ENV_FILE" "$SDKCONFIG"; do
  if [[ ! -f "$f" ]]; then
    echo "Missing file: $f" >&2
    exit 1
  fi
done

ADMIN_SECRET="$(openssl rand -base64 48 | tr -d '\n')"
ROOM_PIN="$(openssl rand -base64 24 | tr -d '\n')"
ROOM_SECRET="$(openssl rand -base64 32 | tr -d '\n')"

backup_file() {
  local f="$1"
  cp "$f" "${f}.bak.$(date +%Y%m%d_%H%M%S)"
}

set_kv_line() {
  local file="$1"
  local key="$2"
  local value="$3"
  if grep -qE "^${key}=" "$file"; then
    sed -i -E "s#^${key}=.*#${key}=${value}#g" "$file"
  else
    printf "%s=%s\n" "$key" "$value" >> "$file"
  fi
}

set_sdkconfig_value() {
  local key="$1"
  local value="$2"
  local quoted="${3:-false}"
  local line
  if [[ "$quoted" == "true" ]]; then
    line="${key}=\"${value}\""
  else
    line="${key}=${value}"
  fi

  if grep -qE "^${key}=" "$SDKCONFIG"; then
    sed -i -E "s#^${key}=.*#${line}#g" "$SDKCONFIG"
  else
    printf "%s\n" "$line" >> "$SDKCONFIG"
  fi
}

if [[ "$ENABLE_BACKUP" == "1" ]]; then
  backup_file "$JANUS_CFG"
  backup_file "$VIDEOROOM_CFG"
  backup_file "$ENV_FILE"
  backup_file "$SDKCONFIG"
fi

# janus.jcfg
sed -i -E "s#^([[:space:]]*admin_secret[[:space:]]*=[[:space:]]*\").*(\".*)#\1${ADMIN_SECRET}\2#g" "$JANUS_CFG"

# janus.plugin.videoroom.jcfg
sed -i -E "0,/^room-[0-9]+:[[:space:]]*\\{/{s/^room-[0-9]+:[[:space:]]*\\{/room-${ROOM_ID}: {/}" "$VIDEOROOM_CFG"
sed -i -E "s#^([[:space:]]*pin[[:space:]]*=[[:space:]]*\").*(\".*)#\1${ROOM_PIN}\2#g" "$VIDEOROOM_CFG"
sed -i -E "s#^([[:space:]]*secret[[:space:]]*=[[:space:]]*\").*(\".*)#\1${ROOM_SECRET}\2#g" "$VIDEOROOM_CFG"

# .env sync for gateway side
set_kv_line "$ENV_FILE" "JANUS_ROOM_ID" "${ROOM_ID}"
set_kv_line "$ENV_FILE" "JANUS_ROOM_PIN" "${ROOM_PIN}"

# sdkconfig sync for firmware side
set_sdkconfig_value "CONFIG_DOORBELL_JANUS_SIGNAL_URL" "${SIGNAL_URL}" true
set_sdkconfig_value "CONFIG_DOORBELL_JANUS_ROOM_ID" "${ROOM_ID}" false
set_sdkconfig_value "CONFIG_DOORBELL_JANUS_ROOM_PIN" "${ROOM_PIN}" true
set_sdkconfig_value "CONFIG_DOORBELL_JANUS_DISPLAY" "${DISPLAY_NAME}" true
set_sdkconfig_value "CONFIG_DOORBELL_MQTT_BROKER_URI" "${MQTT_BROKER_URI}" true

cat <<EOF
Updated successfully:
  - ${JANUS_CFG}
  - ${VIDEOROOM_CFG}
  - ${ENV_FILE}
  - ${SDKCONFIG}

Generated values:
  ROOM_ID=${ROOM_ID}
  ADMIN_SECRET=${ADMIN_SECRET}
  ROOM_PIN=${ROOM_PIN}
  ROOM_SECRET=${ROOM_SECRET}
  SIGNAL_URL=${SIGNAL_URL}
  MQTT_BROKER_URI=${MQTT_BROKER_URI}

Backup mode: $([[ "$ENABLE_BACKUP" == "1" ]] && echo "enabled" || echo "disabled")
EOF
