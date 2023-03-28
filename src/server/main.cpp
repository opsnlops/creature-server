
#include <cstdio>

#include <iostream>
#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include "messaging/server.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using server::CreatureServer;
using server::Creature;
using server::CreatureName;

Status handleGetCreature(ServerContext* context, const CreatureName* request,
                         Creature* reply ) {

    reply->set_name("Beaky");
    reply->set_id("adf");
    reply->set_sacn_ip("10.3.2.11");
    reply->set_universe(1);
    reply->set_dmx_base(1);
    reply->set_number_of_motors(6);

    printf("did a creature\n");

    return Status::OK;
}


class CreatureServerImpl final : public CreatureServer::Service {
    Status GetCreature(ServerContext* context, const CreatureName* request,
                       Creature* reply) override  {
        printf("hello from here\n");
        return handleGetCreature(context, request, reply);
    }
};

void RunServer(uint16_t port) {
    std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
    CreatureServerImpl service;

    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    server->Wait();
    printf("Bye!\n");
}

int main(int argc, char** argv) {
    RunServer(6666);
    return 0;}