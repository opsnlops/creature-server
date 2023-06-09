#!/usr/bin/env bash


echo "Starting local build"

mkdir -p build/

pushd build || exit 1
cmake -DgRPC_ABSL_PROVIDER=module \
      -DgRPC_CARES_PROVIDER=module \
      -DgRPC_PROTOBUF_PROVIDER=module \
      -DgRPC_RE2_PROVIDER=module \
      -DgRPC_SSL_PROVIDER=module \
      -DgRPC_ZLIB_PROVIDER=module \
      -DgRPC_INSTALL=ON \
      -DCMAKE_BUILD_TYPE=Release \
      -G Ninja \
      ..

ninja

popd || exit 1

echo "Finished local build"
