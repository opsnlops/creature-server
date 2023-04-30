#include <string>
#include <sstream>


#include "spdlog/spdlog.h"

#include "messaging/server.pb.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "exception/exception.h"

#include <fmt/format.h>

#include <grpcpp/grpcpp.h>

#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>

#include <bsoncxx/builder/stream/document.hpp>

using server::Creature;
using server::CreatureName;

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;




    grpc::Status Database::updateCreature(const Creature *creature, server::DatabaseInfo *reply) {

        debug("attempting to update a creature in the database");

        auto collection = getCollection(COLLECTION_NAME);
        trace("collection made");

        grpc::Status status;

        debug("name: {}", creature->name().c_str());
        try {
            auto doc_value = creatureToBson(creature, false);
            trace("doc_value made");

            auto filter = bsoncxx::builder::stream::document{} << "_id" << creature->_id()
                                                               << bsoncxx::builder::stream::finalize;
            auto update =
                    bsoncxx::builder::stream::document{} << "$set" << doc_value << bsoncxx::builder::stream::finalize;

            mongocxx::stdx::optional<mongocxx::result::update> result =
                    collection.update_one(filter.view(), update.view());
            trace("update_one done");

            if (result) {
                debug("Matched {} documents", result->matched_count());
                debug("Modified {} documents", result->modified_count());
                reply->set_message("updated the creature in the database");
                status = grpc::Status::OK;
            } else {
                error("No documents matched the filter on update");
                reply->set_message("Unable to update, creature ID not found");
                reply->set_help(fmt::format("ID {} id not found in the database", creature->_id()));
                status = grpc::Status(grpc::StatusCode::NOT_FOUND, "Unable to update, creature ID not found");
            }
        }
        catch (const mongocxx::exception &e) {
            critical("Error updating database: {}", e.what());
            status = grpc::Status(grpc::StatusCode::UNKNOWN, e.what(), fmt::to_string(e.code().value()));
            reply->set_message(
                    fmt::format("Unable to update Creature in database: {} ({})", e.what(), e.code().value()));
            reply->set_help(e.code().message());
        }

        return status;
    }



}
