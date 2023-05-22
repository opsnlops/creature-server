#!/usr/bin/env bash

docker pull opsnlops/creature-server:arm64-dev

CONTAINER_ID=$(docker run -d --rm -p6666:6666 \
    --name creature-server \
    -v /local/sounds:/app/sounds \
    --device /dev/snd \
    --env SOUND_FILE_LOCATION="/app/sounds" \
    --env SOUND_DEVICE_NUMBER=1 \
    --env SOUND_CHANNELS=6 \
    --env SOUND_FREQUENCY=48000 \
    --env SDL_AUDIODRIVER=pulse \
    -v "/run/user/$UID/pulse/native:/run/user/$UID/pulse/native" \
    -v "/usr/share/alsa:/usr/share/alsa" \
    -v "/home/april/.config/pulse:/.config/pulse" \
    --env "PULSE_SERVER=unix:/run/user/$UID/pulse/native" \
    --user "$(id -u)" \
    opsnlops/creature-server:arm64-dev)

docker logs -f $CONTAINER_ID
