
#include <cstdio>
#include <memory>
#include <string>

#include "absl/strings/str_format.h"

#include <grpcpp/grpcpp.h>


#include "messaging/server.grpc.pb.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"


#include "server/database.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using server::CreatureServer;
using server::Creature;
using server::CreatureName;
using server::DatabaseInfo;

using spdlog::info;
using spdlog::debug;
using spdlog::critical;

using creatures::Database;


Database* db{};

Status handleGetCreature(ServerContext* context, const CreatureName* request,
                         Creature* reply ) {

    reply->set_name("Beaky");
    reply->set_id("adf");
    reply->set_sacn_ip("10.3.2.11");
    reply->set_universe(1);
    reply->set_dmx_base(1);
    reply->set_number_of_motors(6);

    debug("did a creature");

    return Status::OK;
}

Status handleSave(ServerContext* context, const Creature* request, DatabaseInfo* reply) {

    debug("asking the server to save maybe?");
    return db->saveCreature(request, reply);
}


class CreatureServerImpl final : public CreatureServer::Service {

    Status GetCreature(ServerContext* context, const CreatureName* request,
                       Creature* reply) override  {
        debug("hello from get");
        return handleGetCreature(context, request, reply);
    }

    Status SaveCreature(ServerContext* context, const Creature* creature,
                       DatabaseInfo* reply) override  {
        debug("hello from save");
        return handleSave(context, creature, reply);
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
    info("Server listening on {}", server_address);

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    server->Wait();
    printf("Bye!\n");
}

int main(int argc, char** argv) {

    spdlog::set_level(spdlog::level::trace);

    // Start up the database
    db = new Database();



    info("starting server on port {}", 6666);
    RunServer(6666);
    return 0;
}