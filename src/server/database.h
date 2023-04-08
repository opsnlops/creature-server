
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
#define COLLECTION_NAME "creatures"

using server::Creature;
using server::CreatureName;
using server::CreatureId;

#include <grpcpp/grpcpp.h>
#include "messaging/server.pb.h"

namespace creatures {

    class Database {

    public:
        explicit Database(mongocxx::pool &pool);

        grpc::Status createCreature(const Creature* creature, server::DatabaseInfo* reply);
        grpc::Status updateCreature(const Creature* creature, server::DatabaseInfo* reply);
        grpc::Status searchCreatures(const CreatureName* creatureName, Creature* creature);
        grpc::Status getCreature(const CreatureId* creatureId, Creature* creature);



        /**
         * Ping the database to make sure it's alive
         *
         * @return Status::OK if the DB is okay
         */
        grpc::Status ping();

    private:
        mongocxx::pool& pool;
        mongocxx::collection getCollection(const std::string& collectionName);
        static bsoncxx::document::value creatureToBson(const Creature* creature, bool assignNewId);
        static void creatureFromBson(const bsoncxx::document::value& doc, Creature* creature);
        static std::chrono::system_clock::time_point protobufTimestampToTimePoint(const google::protobuf::Timestamp& timestamp);
        static google::protobuf::Timestamp convertMongoDateToProtobufTimestamp(const bsoncxx::document::element& mongo_timestamp_element);
    };


}
