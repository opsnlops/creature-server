#!/usr/bin/env bash

# build oatpp
mkdir -p externals/build
mkdir -p externals/install

cd externals/build
cmake .. -DCMAKE_INSTALL_PREFIX=../install -GNinja
cmake --build .
