
#include <string>

#include "spdlog/spdlog.h"

#include "messaging/server.pb.h"
#include "server/database.h"

#include <grpcpp/grpcpp.h>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>

#include <google/protobuf/timestamp.pb.h>
#include <chrono>


using server::Creature;
using server::CreatureName;

using spdlog::info;
using spdlog::debug;
using spdlog::critical;
using spdlog::trace;

using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    Database::Database(mongocxx::pool &pool) : pool(pool) {

        info("starting up database connection");
    }


    mongocxx::collection Database::getCollection(std::string collectionName) {

        debug("connecting to a collection");

        // Acquire a MongoDB client from the pool
        auto client = pool.acquire();
        auto collection = (*client)[DB_NAME][collectionName];

        return collection;

    }

    grpc::Status Database::ping() {

        // Ping the database.
        const auto ping_cmd = make_document(kvp("ping", 1));

        //auto client = pool.acquire();
        //mongocxx::database db = client[DB_NAME];
        //db.run_command(ping_cmd.view());

        return grpc::Status::OK;

    }

    grpc::Status Database::createCreature(const Creature* creature, server::DatabaseInfo* reply) {

        debug("attempting to save a creature in the database");

        auto collection = getCollection("creatures");
        trace("collection made");

        grpc::Status status;

        debug("name: {}", creature->name().c_str());
        try {
            auto doc_value = creatureToBson(creature);
            trace("doc_value made");
            collection.insert_one(doc_value.view());
            trace("run_command done");

            info("save something in the database maybe?");

            status = grpc::Status::OK;
            reply->set_message("saved thingy in the thingy");


        }
        catch (const std::exception& e)
        {

            critical("Unable to connect to database: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL, e.what());
            reply->set_message("well shit");
            reply->set_help(e.what());

        }

        return status;
    }

    /*
     *   string name = 1;
  string id = 2;    // MongoDB _id field
  google.protobuf.Timestamp last_updated = 3;
  string sacn_ip = 4;
  uint32 universe = 5;
  uint32 dmx_base = 6;
  uint32 number_of_motors = 7;
     */

    bsoncxx::document::value Database::creatureToBson(const Creature* creature) {
        using bsoncxx::builder::stream::document;
        using bsoncxx::builder::stream::finalize;
        using std::chrono::system_clock;


        // Convert the protobuf Timestamp to a std::chrono::system_clock::time_point
        system_clock::time_point last_updated = protobufTimestampToTimePoint(creature->last_updated());


        document builder{};

        builder << "name" << creature->name()
                << "_id" << creature->id()
                << "sacn_ip" << creature->sacn_ip()
                << "last_updated" << bsoncxx::types::b_date{last_updated}
                << "universe" << (std::int32_t)creature->universe()
                << "dmx_base" << (std::int32_t)creature->dmx_base()
                << "number_of_motors" << (std::int32_t)creature->number_of_motors();

        return builder << finalize;
    }

    std::chrono::system_clock::time_point Database::protobufTimestampToTimePoint(const google::protobuf::Timestamp& timestamp) {
        using std::chrono::duration_cast;
        using std::chrono::nanoseconds;
        using std::chrono::seconds;
        using std::chrono::system_clock;

        auto duration = seconds{timestamp.seconds()} + nanoseconds{timestamp.nanos()};
        return system_clock::time_point{duration_cast<system_clock::duration>(duration)};
    }


}