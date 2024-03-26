
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

    Status CreatureServerImpl::CreateCreature(ServerContext *context, const Creature *creature, DatabaseInfo *reply) {debug("hello from save");

        debug("attempting to save a new creature");
        try {
            db->createCreature(creature, reply);
            return grpc::Status(grpc::StatusCode::OK, "✅ Created new creature");
        }
        catch (const creatures::InvalidArgumentException &e) {
            error("Invalid argument exception while creating a creature: {}", e.what());
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what());
        }
        catch (const creatures::InternalError &e) {
            error("Internal error while creating a creature: {}", e.what());
            return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
        }
        catch (...) {
            error("Unknown error while creating a creature");
            return grpc::Status(grpc::StatusCode::INTERNAL, "Unknown error");
        }
    }


    void Database::createCreature(const Creature *creature, server::DatabaseInfo *reply) {

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

            reply->set_message("created new creature in the database");

        }
        catch (const mongocxx::exception &e) {
            // Was this an attempt to make a duplicate creature?
            if (e.code().value() == 11000) {
                std::string error_message = fmt::format("attempted to insert a duplicate Creature in the database for id {}",
                                                        creature->_id());

                reply->set_message("Unable to create new creature");
                reply->set_help(fmt::format("ID {} already exists", creature->_id()));
                throw creatures::InvalidArgumentException(error_message);

            } else {

                std::string error_message = fmt::format("Error in the database while adding a creature: {} ({})",
                                                        e.what(), e.code().value());
                critical(error_message);
                status = grpc::Status(grpc::StatusCode::UNKNOWN, e.what(), fmt::to_string(e.code().value()));
                reply->set_message(error_message);
                reply->set_help(e.code().message());
                throw creatures::InternalError(error_message);
            }
        }

        catch (...) {
            std::string error_message = "Unknown error while adding a creature to the database";
            critical(error_message);
            reply->set_message(error_message);
            throw creatures::InternalError(error_message);
        }

    }

}