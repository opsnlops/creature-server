#!/usr/bin/env bash

set -euo pipefail

SERVER_BINARY=${CREATURE_SERVER_BINARY:-/usr/bin/creature-server}
export SOUND_FILE_LOCATION=${SOUND_FILE_LOCATION:-/local/sounds}

exec "${SERVER_BINARY}" \
    --travel-mode \
    --audio-device-name "${SOUND_DEVICE_NAME:-plughw:CARD=S3,DEV=0}" \
    --sound-channels 2 \
    --sound-frequency 48000 \
    "$@"
