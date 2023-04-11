
#pragma once

#include <grpcpp/grpcpp.h>

#include "absl/strings/str_format.h"

#include "messaging/server.pb.h"
#include "messaging/server.grpc.pb.h"
#include "server/database.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using server::CreatureServer;
using server::Creature;
using server::CreatureName;
using server::DatabaseInfo;
using server::CreatureId;
using server::CreatureFilter;
using server::ListCreaturesResponse;
using server::GetAllCreaturesResponse;

namespace creatures {

    class CreatureServerImpl final : public CreatureServer::Service {

        Status SearchCreatures(ServerContext *context, const CreatureName *request, Creature *reply) override;

        Status CreateCreature(ServerContext *context, const Creature *creature, DatabaseInfo *reply) override;

        Status GetCreature(ServerContext *context, const CreatureId *id, Creature *reply) override;

        Status
        ListCreatures(ServerContext *context, const CreatureFilter *filter, ListCreaturesResponse *response) override;

        Status GetAllCreatures(ServerContext *context, const CreatureFilter *filter,
                               GetAllCreaturesResponse *response) override;
    };


    Status handleSearchCreatures(ServerContext *context, const CreatureName *request, Creature *reply);

    Status handleListCreatures(ServerContext *context, const CreatureFilter *filter, ListCreaturesResponse *response);

    Status
    handleGetAllCreatures(ServerContext *context, const CreatureFilter *filter, GetAllCreaturesResponse *response);

    Status handleGetCreature(ServerContext *context, const CreatureId *id, Creature *reply);

    Status handleSave(ServerContext *context, const Creature *request, DatabaseInfo *reply);


}


