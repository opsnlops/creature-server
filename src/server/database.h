
#pragma once

#include <string>

#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>


// TODO: Clean this up
#define DB_URI  "mongodb://10.3.2.11"

using server::Creature;
using server::CreatureName;

using spdlog::info;
using spdlog::debug;
using spdlog::critical;

#include <grpcpp/grpcpp.h>
#include "messaging/server.pb.h"

namespace creatures {

    class Database {

    public:
        Database();

        grpc::Status saveCreature(const Creature* creature, server::DatabaseInfo* reply);
        Creature getCreature(CreatureName name);

    private:
        mongocxx::database db;

    };


}
