#!/usr/bin/env bash

DOCKER_BUILDKIT=1 docker build . -t opsnlops/creature-server:latest
