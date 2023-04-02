
#include <string>

#include "spdlog/spdlog.h"

#include "messaging/server.pb.h"
#include "server/database.h"

#include <grpcpp/grpcpp.h>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/array.hpp>

#include <google/protobuf/timestamp.pb.h>
#include <chrono>

#include <fmt/format.h>

using server::Creature;
using server::CreatureName;

using spdlog::info;
using spdlog::debug;
using spdlog::error;
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
        catch (const mongocxx::exception& e)
        {
            // Was this an attempt to make a duplicate creature?
            if(e.code().value() == 11000) {
                error("attempted to insert a duplicate Creature in the database for name {}", creature->name());
                status = grpc::Status(grpc::StatusCode::ALREADY_EXISTS, e.what());
                reply->set_message("Unable to create new creature");
                reply->set_help(fmt::format("Name {} already exists", creature->name()));

            }

            else {
                critical("Error updating database: {}", e.what());
                status = grpc::Status(grpc::StatusCode::UNKNOWN, e.what(), fmt::to_string(e.code().value()));
                reply->set_message(fmt::format("Unable to create Creature in database: {} ({})", e.what(), e.code().value()));
                reply->set_help(e.code().message());
            }

        }

        return status;
    }

    bsoncxx::document::value Database::creatureToBson(const Creature* creature) {
        using bsoncxx::builder::stream::document;
        using bsoncxx::builder::stream::finalize;
        using std::chrono::system_clock;

        debug("converting a creature to bson");

        bsoncxx::builder::stream::document builder{};
        trace("builder made");

        // Convert the protobuf Timestamp to a std::chrono::system_clock::time_point
        system_clock::time_point last_updated = protobufTimestampToTimePoint(creature->last_updated());
        try {

            builder << "_id" << creature->name()
                    << "sacn_ip" << creature->sacn_ip()
                    << "last_updated" << bsoncxx::types::b_date{last_updated}
                    << "universe" << bsoncxx::types::b_int32{static_cast<int32_t>(creature->universe())}
                    << "dmx_base" << bsoncxx::types::b_int32{static_cast<int32_t>(creature->dmx_base())}
                    << "number_of_motors" << bsoncxx::types::b_int32{static_cast<int32_t>(creature->number_of_motors())};
            trace("non-array fields added");

            auto array_builder = builder << "motors" << bsoncxx::builder::stream::open_array;
            trace("array_builder made");
            for (int j = 0; j < creature->motors_size(); j++) {

                const server::Creature_Motor &motor = creature->motors(j);
                trace("adding motor {}", j);

                array_builder = array_builder << bsoncxx::builder::stream::open_document
                                              << "type" << server::Creature_MotorType_Name(motor.type())
                                              << "number" << bsoncxx::types::b_int32{static_cast<int32_t>(motor.number())}
                                              << "max_value" << bsoncxx::types::b_int32{static_cast<int32_t>(motor.max_value())}
                                              << "mix_value" << bsoncxx::types::b_int32{static_cast<int32_t>(motor.min_value())}
                                              << "smoothing_value" << bsoncxx::types::b_double{static_cast<double>(motor.smoothing_value())}
                                              << bsoncxx::builder::stream::close_document;
            }
            trace("done adding motors");
            array_builder << bsoncxx::builder::stream::close_array;

        }
        catch(const mongocxx::exception& e)
        {
            error("Problems making the document: {}", e.what());
        }

        // All done, close up the doc!
        bsoncxx::document::value doc = builder.extract();
        trace("extract done");

        // Log this so I can see what was made
        debug("Built doc: {}", bsoncxx::to_json(doc));

        return doc;
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