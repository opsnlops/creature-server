#!/usr/bin/env bash

docker pull opsnlops/creature-server:arm64

CONTAINER_ID=$(docker run -d --rm -p6666:6666 \
	--name creature-server \
	-v /local/sounds:/app/sounds \
	--device /dev/snd \
	--device /dev/gpiomem \
	--env SOUND_FILE_LOCATION="/app/sounds" \
	--env SOUND_DEVICE_NUMBER=0 \
	--env SOUND_CHANNELS=6 \
	opsnlops/creature-server:arm64)

docker logs -f $CONTAINER_ID
