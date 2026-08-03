#!/usr/bin/env bash

set -euo pipefail

SERVER_BINARY=${CREATURE_SERVER_BINARY:-/usr/bin/creature-server}
export SOUND_FILE_LOCATION=${SOUND_FILE_LOCATION:-/local/sounds}

exec "${SERVER_BINARY}" "$@"
