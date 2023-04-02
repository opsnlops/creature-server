
#pragma once

#include <string>

#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>


#include <google/protobuf/timestamp.pb.h>
#include <chrono>

// TODO: Clean this up
#define DB_URI  "mongodb://10.3.2.11"
#define DB_NAME  "creature_server"

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
        explicit Database(mongocxx::pool &pool);

        grpc::Status createCreature(const Creature* creature, server::DatabaseInfo* reply);
        grpc::Status updateCreature(const Creature* creature, server::DatabaseInfo* reply);
        Creature getCreature(CreatureName name);

        /**
         * Ping the database to make sure it's alive
         *
         * @return Status::OK if the DB is okay
         */
        grpc::Status ping();

    private:
        mongocxx::pool& pool;
        mongocxx::collection getCollection(std::string collectionName);
        bsoncxx::document::value creatureToBson(const Creature* creature);
        std::chrono::system_clock::time_point protobufTimestampToTimePoint(const google::protobuf::Timestamp& timestamp);
    };


}
