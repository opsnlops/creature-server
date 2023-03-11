/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

// Package main implements a client for Greeter service.
package main

import (
	"context"
	"flag"
	"fmt"
	"github.com/honeycombio/honeycomb-opentelemetry-go"
	"github.com/honeycombio/otel-launcher-go/launcher"
	"go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc"
	"google.golang.org/protobuf/types/known/emptypb"
	"log"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	pb "opsnlops.io/creatures/grpc"
)

const (
	defaultName = "April"
)

var (
	addr = flag.String("addr", "localhost:50051", "the address to connect to")
	name = flag.String("name", defaultName, "Name to greet")
)

func main() {

	bsp := honeycomb.NewBaggageSpanProcessor()

	// use honeycomb distro to setup OpenTelemetry SDK
	otelShutdown, err := launcher.ConfigureOpenTelemetry(
		launcher.WithSpanProcessor(bsp),
		launcher.WithServiceName("creature-client"),
	)
	if err != nil {
		log.Fatalf("error setting up OTel SDK - %e", err)
	}
	defer otelShutdown()

	flag.Parse()
	// Set up a connection to the server.
	conn, err := grpc.Dial(*addr, grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithUnaryInterceptor(otelgrpc.UnaryClientInterceptor()),
		grpc.WithStreamInterceptor(otelgrpc.StreamClientInterceptor()))
	if err != nil {
		log.Fatalf("did not connect: %v", err)
	}
	defer conn.Close()
	c := pb.NewCreatureServerClient(conn)

	// Contact the server and print out its response.
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	_, err = c.GetCreature(ctx, &pb.CreatureName{Name: *name})
	if err != nil {
		log.Fatalf("could not get Bob: %v", err)
	}

	// Get all of them
	fmt.Println("Getting all of them")
	_, err = c.GetCreatures(ctx, &emptypb.Empty{})
	if err != nil {
		log.Fatalf("could not everyone: %v", err)
	}

	//log.Printf("Greeting: %s", r.Creature())
}
