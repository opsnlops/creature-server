#!/usr/bin/env bash

set -euo pipefail

IMAGE_NAME=creature-server-buildtest-arm64

docker buildx build --platform linux/arm64 --target build --tag "${IMAGE_NAME}" --load .
docker run --rm --workdir /build/creature-server/build "${IMAGE_NAME}" \
    /build/creature-server/run_linux_tests.sh /build/creature-server/build
