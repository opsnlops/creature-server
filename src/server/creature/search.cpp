
#include "server/config.h"

#include <string>

#include "spdlog/spdlog.h"

#include "exception/exception.h"
#include "messaging/server.pb.h"
#include "server/database.h"
#include "server/creature-server.h"


#include <fmt/format.h>

#include <grpcpp/grpcpp.h>

#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>


#include <bsoncxx/builder/stream/document.hpp>

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;

    Status CreatureServerImpl::SearchCreatures(ServerContext *context, const CreatureName *request, Creature *reply) {
        debug("calling searching for a creature");

        grpc::Status status;

        try {
            db->searchCreatures(request, reply);
            debug("creature {} found in DB!", request->name());
            status = grpc::Status(grpc::StatusCode::OK,
                                  fmt::format("✅ Searched for creature name '{}' successfully!", request->name()));
            return status;

        }
        catch (const creatures::NotFoundException &e) {
            info("creature {} not found", request->name());
            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                  e.what(),
                                  fmt::format("🚫 Creature name '{}' not found", request->name()));
            return status;
        }
        catch (const creatures::DataFormatException &e) {
            critical("Data format exception while looking for a creature: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  e.what(),
                                  "A data formatting error occurred while looking for this creature");
            return status;
        }
        catch (const creatures::InvalidArgumentException &e) {
            error("an empty name was passed into searchCreatures()");
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                  e.what(),
                                  fmt::format("⚠️ A creature name must be supplied"));
            return status;
        }

    }

    grpc::Status Database::searchCreatures(const CreatureName *creatureName, Creature *creature) {

        grpc::Status status;
        if (creatureName->name().empty()) {
            info("an attempt to search for an empty name was made");
            throw InvalidArgumentException("unable to search for Creatures because the name was empty");
        }

        // Okay, we know we have a non-empty name
        std::string name = creatureName->name();
        debug("attempting to search for a creature named {}", name);

        auto collection = getCollection(CREATURES_COLLECTION);
        trace("collection located");

        try {
            // Create a filter BSON document to match the target document
            auto filter = bsoncxx::builder::stream::document{} << "name" << name << bsoncxx::builder::stream::finalize;

            // Find the document with the matching _id field
            bsoncxx::stdx::optional<bsoncxx::document::value> result = collection.find_one(filter.view());

            if (!result) {
                info("no creatures named '{}' found", name);
                throw creatures::NotFoundException(fmt::format("no creatures named '{}' found", name));
            }

            // Unwrap the optional to obtain the bsoncxx::document::value
            bsoncxx::document::value found_document = *result;
            creatureFromBson(found_document, creature);

            debug("find completed!");

            return grpc::Status::OK;
        }
        catch (const mongocxx::exception &e) {
            critical("an unhandled error happened while searching for a creature: {}", e.what());
            throw InternalError(
                    fmt::format("an unhandled error happened while searching for a creature: {}", e.what()));
        }
    }


}