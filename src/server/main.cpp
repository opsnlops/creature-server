
#include <cstdio>
#include <memory>
#include <string>

#include "absl/strings/str_format.h"

#include <grpcpp/grpcpp.h>


#include "messaging/server.grpc.pb.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "server/database.h"
#include "exception/exception.h"

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

    debug("handleGetCreature() time!");

    grpc::Status status;

    try {
         db->getCreature(request->name(), reply);
         debug("creature {} found in DB!", request->name());
        status = grpc::Status(grpc::StatusCode::OK,
                              fmt::format("Loaded creature {} successfully!", request->name()));
        return status;

    }
    catch(const creatures::CreatureNotFoundException& e) {
        info("creature {} not found", request->name());
        status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                              e.what(),
                              fmt::format("Creature name '{}' not found", request->name()));
        return status;
    }
    catch(const creatures::DataFormatException& e) {
        critical("Data format exception while looking for a creature: {}", e.what());
        status = grpc::Status(grpc::StatusCode::INTERNAL,
                              e.what(),
                              "A data formatting error occured while looking for this creature");
        return status;
    }


}

Status handleSave(ServerContext* context, const Creature* request, DatabaseInfo* reply) {

    debug("asking the server to save maybe?");
    return db->createCreature(request, reply);
}


class CreatureServerImpl final : public CreatureServer::Service {

    Status GetCreature(ServerContext* context, const CreatureName* request,
                       Creature* reply) override  {
        debug("hello from get");
        return handleGetCreature(context, request, reply);
    }

    Status CreateCreature(ServerContext* context, const Creature* creature,
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

    server->Wait();
    info("Bye!");
}

int main(int argc, char** argv) {

    spdlog::set_level(spdlog::level::trace);

    // Fire up the Mono client
    mongocxx::instance instance{};
    mongocxx::uri uri(DB_URI);
    mongocxx::pool mongo_pool(uri);

    // Start up the database
    db = new Database(mongo_pool);


    info("starting server on port {}", 6666);
    RunServer(6666);
    return 0;
}