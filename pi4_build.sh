#!/usr/bin/env bash

docker buildx build --platform linux/arm64 -t opsnlops/creature-server:arm64-dev .
docker push opsnlops/creature-server:arm64-dev


