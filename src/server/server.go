package main

import (
	"context"
	"flag"
	"fmt"
	"github.com/honeycombio/honeycomb-opentelemetry-go"
	"github.com/honeycombio/otel-launcher-go/launcher"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo"
	"go.mongodb.org/mongo-driver/mongo/options"
	grpcotel "go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc"
	"google.golang.org/grpc"
	"google.golang.org/protobuf/types/known/emptypb"
	"log"
	"net"
	pb "opsnlops.io/creatures/grpc"
)

var (
	port = flag.Int("port", 50051, "The server port")
)

type server struct {
	pb.UnimplementedCreatureServerServer
}

func (s *server) GetCreature(ctx context.Context, in *pb.CreatureName) (*pb.Creature, error) {
	log.Printf("Received: %v", in.GetName())
	return &pb.Creature{}, nil
}

func (s *server) GetCreatures(in *emptypb.Empty, stream pb.CreatureServer_GetCreaturesServer) error {
	log.Printf("Getting all creatures")

	coll := mongoConn.Database("fakeDatabase").Collection("movies")
	title := "Back to the Future"
	var result bson.M
	err := coll.FindOne(context.TODO(), bson.D{{"title", title}}).Decode(&result)
	if err == mongo.ErrNoDocuments {
		fmt.Printf("No document was found with the title %s\n", title)
	}

	return nil
}

var mongoConn *mongo.Client

func main() {

	// enable multi-span attributes
	bsp := honeycomb.NewBaggageSpanProcessor()

	// use honeycomb distro to setup OpenTelemetry SDK
	otelShutdown, err := launcher.ConfigureOpenTelemetry(
		launcher.WithSpanProcessor(bsp),
		launcher.WithServiceName("creature-server"),
	)
	if err != nil {
		log.Fatalf("error setting up OTel SDK - %e", err)
	}
	defer otelShutdown()

	flag.Parse()
	lis, err := net.Listen("tcp", fmt.Sprintf(":%d", *port))
	if err != nil {
		log.Fatalf("failed to listen: %v", err)
	}

	mongoConn, err = mongo.Connect(context.TODO(), options.Client().ApplyURI("mongodb://10.3.2.11"))
	if err != nil {
		panic(err)
	}
	defer func() {
		if err := mongoConn.Disconnect(context.TODO()); err != nil {
			panic(err)
		}
	}()

	s := grpc.NewServer(
		grpc.UnaryInterceptor(grpcotel.UnaryServerInterceptor()),
		grpc.StreamInterceptor(grpcotel.StreamServerInterceptor()),
	)

	pb.RegisterCreatureServerServer(s, &server{})
	log.Printf("server listening at %v", lis.Addr())
	if err := s.Serve(lis); err != nil {
		log.Fatalf("failed to serve: %v", err)
	}
}
