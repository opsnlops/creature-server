#!/usr/bin/env bash

set -euo pipefail

echo "Starting local build"

if [[ -x "./build_oatpp.sh" ]]; then
  ./build_oatpp.sh
fi

if [[ -f "build/CMakeCache.txt" ]]; then
  if ! grep -q "CMAKE_GENERATOR:INTERNAL=Ninja" "build/CMakeCache.txt"; then
    echo "Cleaning build directory for Ninja generator"
    rm -rf build
  fi
fi

mkdir -p build/

NINJA_FLAGS=${NINJA_FLAGS:-}

pushd build || exit 1
cmake -DCMAKE_BUILD_TYPE=Debug \
      -G Ninja \
      ..

ninja ${NINJA_FLAGS}

popd || exit 1

echo "Finished local build"
