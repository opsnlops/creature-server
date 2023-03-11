#!/usr/bin/env bash



pushd src

mkdir -p grpc

protoc --go_out=grpc/ \
       --go_opt=paths=source_relative \
       --go-grpc_out=grpc/ \
       --go-grpc_opt=paths=source_relative \
       -I ../messaging \
       ../messaging/server.proto

mkdir -p ../bin

go build -o ../bin/client client/client.go
go build -o ../bin/server server/server.go

ls -lart ../bin

popd