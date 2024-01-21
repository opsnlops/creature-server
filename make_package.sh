#!/usr/bin/env bash

#
# Build an image the same way the GHA build does
#

docker build -f Dockerfile . --target package -t temp-image
docker create --name temp-container temp-image
mkdir -p package/ && docker cp temp-container:/package/ .
docker rm temp-container

ls -lart package/
