
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

#include "quill/Quill.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using server::CreatureServer;
using server::Creature;
using server::CreatureName;

Status handleGetCreature(ServerContext* context, const CreatureName* request,
                         Creature* reply ) {
    quill::Logger* logger = quill::get_logger();

    reply->set_name("Beaky");
    reply->set_id("adf");
    reply->set_sacn_ip("10.3.2.11");
    reply->set_universe(1);
    reply->set_dmx_base(1);
    reply->set_number_of_motors(6);

    LOG_DEBUG(logger, "did a creature");

    return Status::OK;
}


class CreatureServerImpl final : public CreatureServer::Service {
    quill::Logger* logger = quill::get_logger();

    Status GetCreature(ServerContext* context, const CreatureName* request,
                       Creature* reply) override  {
        LOG_DEBUG(logger, "hello from here");
        return handleGetCreature(context, request, reply);
    }
};

void RunServer(uint16_t port) {
    quill::Logger* logger = quill::get_logger();
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
    LOG_INFO(logger, "Server listening on {}", server_address);

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    server->Wait();
    printf("Bye!\n");
}

int main(int argc, char** argv) {

    quill::Config cfg;
    cfg.enable_console_colours = true;
    quill::configure(cfg);
    quill::start();

    quill::Logger* logger = quill::get_logger();
    logger->set_log_level(quill::LogLevel::TraceL3);

    // enable a backtrace that will get flushed when we log CRITICAL
    logger->init_backtrace(2, quill::LogLevel::Critical);

    LOG_INFO(logger, "starting server on point {}", 6666);
    RunServer(6666);
    return 0;
}