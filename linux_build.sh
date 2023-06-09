#!/usr/bin/env bash

# DOCKER_BUILDKIT=1 docker buildx --platform linux/amd64,linux/arm64 . -t opsnlops/creature-server:dev

DOCKER_BUILDKIT=1 docker build . -t opsnlops/creature-server:dev

#DOCKER_BUILDKIT=1 docker build . -t opsnlops/creature-server:dev
