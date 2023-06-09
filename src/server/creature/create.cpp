
#include "server/config.h"

#include <string>
#include "spdlog/spdlog.h"

#include "server.pb.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "exception/exception.h"

#include <fmt/format.h>

#include <grpcpp/grpcpp.h>

#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>


#include <bsoncxx/builder/stream/document.hpp>

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;

    Status CreatureServerImpl::CreateCreature(ServerContext *context, const Creature *creature, DatabaseInfo *reply) {
        debug("hello from save");

        debug("asking the server to save maybe?");
        return db->createCreature(creature, reply);
    }


    grpc::Status Database::createCreature(const Creature *creature, server::DatabaseInfo *reply) {

        info("attempting to save a creature in the database");

        auto collection = getCollection(CREATURES_COLLECTION);
        trace("collection made");

        grpc::Status status;

        debug("name: {}", creature->name().c_str());
        try {
            auto doc_value = creatureToBson(creature, true);
            trace("doc_value made");
            collection.insert_one(doc_value.view());
            trace("run_command done");

            info("saved creature in the database");

            status = grpc::Status::OK;
            reply->set_message("saved thingy in the thingy");

        }
        catch (const mongocxx::exception &e) {
            // Was this an attempt to make a duplicate creature?
            if (e.code().value() == 11000) {
                error("attempted to insert a duplicate Creature in the database for id {}", creature->_id());
                status = grpc::Status(grpc::StatusCode::ALREADY_EXISTS, e.what());
                reply->set_message("Unable to create new creature");
                reply->set_help(fmt::format("ID {} already exists", creature->_id()));
            } else {
                critical("Error updating database: {}", e.what());
                status = grpc::Status(grpc::StatusCode::UNKNOWN, e.what(), fmt::to_string(e.code().value()));
                reply->set_message(
                        fmt::format("Unable to create Creature in database: {} ({})", e.what(), e.code().value()));
                reply->set_help(e.code().message());
            }

        }

        return status;
    }

}