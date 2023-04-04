#!/usr/bin/env bash

export SRC_DIR=messaging
export DST_DIR=src/messaging

# CPP
protoc -I=$SRC_DIR --cpp_out=$DST_DIR $SRC_DIR/server.proto
protoc -I=$SRC_DIR --grpc_out=$DST_DIR --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) $SRC_DIR/server.proto

# Swift
protoc --swift_out=swift/ $SRC_DIR/server.proto
protoc --grpc-swift_out=swift/ --grpc-swift_opt=Client=true,Server=false $SRC_DIR/server.proto
