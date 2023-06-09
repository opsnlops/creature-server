#!/usr/bin/env bash

export SRC_DIR=messaging

# Swift
protoc --swift_out=swift/ --swift_opt=Visibility=Public $SRC_DIR/server.proto
protoc --grpc-swift_out=swift/ --grpc-swift_opt=Visibility=Public --grpc-swift_opt=Client=true,Server=false $SRC_DIR/server.proto
