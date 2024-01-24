#!/usr/bin/env bash


echo "Starting local build"

mkdir -p build/

pushd build || exit 1
cmake -DCMAKE_BUILD_TYPE=Debug \
      -G Ninja \
      ..

ninja

popd || exit 1

echo "Finished local build"
