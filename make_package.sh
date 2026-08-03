#!/usr/bin/env bash

set -euo pipefail

PACKAGE_PLATFORM=${1:-linux/amd64}
PACKAGE_IMAGE=creature-server-package-local
PACKAGE_CONTAINER="creature-server-package-copy-$$"

docker buildx build --platform "${PACKAGE_PLATFORM}" --file Dockerfile \
    --target package --tag "${PACKAGE_IMAGE}" --load .
docker create --name "${PACKAGE_CONTAINER}" "${PACKAGE_IMAGE}"
trap 'docker rm --force "${PACKAGE_CONTAINER}" >/dev/null 2>&1 || true' EXIT
mkdir -p package
docker cp "${PACKAGE_CONTAINER}:/package/." package/

ls -lart package/
