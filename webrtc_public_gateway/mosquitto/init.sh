#!/bin/sh
set -eu

if [ -z "${JANUS_ROOM_PIN:-}" ]; then
  echo "JANUS_ROOM_PIN is empty; cannot bootstrap MQTT auth"
  exit 1
fi

MQTT_USER="${MQTT_USERNAME:-${JANUS_ROOM_ID:-}}"
if [ -z "${MQTT_USER}" ]; then
  echo "MQTT username is empty; JANUS_ROOM_ID is required"
  exit 1
fi
PASSFILE="/mosquitto/data/passwords"
mkdir -p /mosquitto/data /mosquitto/log
chmod 755 /mosquitto /mosquitto/data /mosquitto/log || true

if [ -f "${PASSFILE}" ]; then
  /usr/bin/mosquitto_passwd -b "${PASSFILE}" "${MQTT_USER}" "${JANUS_ROOM_PIN}"
else
  /usr/bin/mosquitto_passwd -b -c "${PASSFILE}" "${MQTT_USER}" "${JANUS_ROOM_PIN}"
fi
chown 1883:1883 "${PASSFILE}" /mosquitto/data /mosquitto/log || true
chmod 640 "${PASSFILE}" || true
exec /usr/sbin/mosquitto -c /mosquitto/config/mosquitto.conf
