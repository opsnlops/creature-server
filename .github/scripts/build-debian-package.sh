#!/usr/bin/env bash

set -euo pipefail

cd /workspace

git config --global --add safe.directory /workspace

./build_oatpp.sh

cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

cmake --build build --parallel 4
./run_linux_tests.sh build

cpack --config build/CPackConfig.cmake -G DEB -B package
dpkg-deb --info package/*.deb
