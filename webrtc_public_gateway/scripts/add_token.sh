#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "Usage: $0 <admin-secret> <token> <janus-admin-url>"
  echo "Example: $0 S3cret viewer_token_1 http://127.0.0.1:7088/admin"
  exit 1
fi

ADMIN_SECRET="$1"
TOKEN="$2"
ADMIN_URL="$3"

curl -sS -X POST "$ADMIN_URL" \
  -H 'Content-Type: application/json' \
  -d "{\"janus\":\"add_token\",\"admin_secret\":\"${ADMIN_SECRET}\",\"token\":\"${TOKEN}\",\"plugins\":[\"janus.plugin.videoroom\"],\"transaction\":\"addtoken$(date +%s)\"}" | jq .
