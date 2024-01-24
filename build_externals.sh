#!/usr/bin/env bash

echo "Starting externals build"

mkdir -p external/build/

pushd external/build || exit 1
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=../install \
      -G Ninja \
      ..
ninja
ninja install

popd || exit 1

echo "Finished externals build"
