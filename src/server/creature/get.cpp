
#include "server/config.h"

#include <string>

#include "spdlog/spdlog.h"

#include "server.pb.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "exception/exception.h"

#include <fmt/format.h>

#include <grpcpp/grpcpp.h>


#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>

#include <bsoncxx/builder/stream/document.hpp>

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;

    Status CreatureServerImpl::GetCreature(ServerContext *context, const CreatureId *id, Creature *reply) {
        debug("calling getCreature()");

        grpc::Status status;
        try {
            db->getCreature(id, reply);
            debug("creature found in DB on get!");
            status = grpc::Status(grpc::StatusCode::OK,
                                  fmt::format("✅ Got creature by id successfully!"));
            return status;

        }
        catch (const creatures::NotFoundException &e) {
            info("creature id not found");
            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                  e.what(),
                                  fmt::format("🚫 Creature id not found"));
            return status;
        }
        catch (const creatures::DataFormatException &e) {
            critical("Data format exception while getting a creature by id: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  e.what(),
                                  "A data formatting error occurred while looking for a creature by id");
            return status;
        }
        catch (const creatures::InvalidArgumentException &e) {
            error("an empty name was passed into getCreature()");
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                  e.what(),
                                  fmt::format("⚠️ A creature id must be supplied"));
            return status;
        }
    }



    /**
     * Get a creature from the database
     *
     * @param creatureId The creature ID to look up
     * @param creature A pointer to a Creature to fill out
     *
     * @throws InvalidArgumentException if creatureID is empty
     * @throws CreatureNotFoundException if the creature ID is not found
     * @throws InternalError if a database error occurs
     *
     */
    void Database::getCreature(const CreatureId *creatureId, Creature *creature) {

        if (creatureId->_id().empty()) {
            info("an empty creatureID was passed into getCreature()");
            throw InvalidArgumentException("unable to get a creature because the id was empty");
        }

        // Convert the ID into MongoID's ID
        bsoncxx::oid id = bsoncxx::oid(creatureId->_id().data(), bsoncxx::oid::k_oid_length);
        debug("attempting to search for a creature by ID: {}", id.to_string());

        auto collection = getCollection(CREATURES_COLLECTION);
        trace("collection located");

        try {

            // Create a filter BSON document to match the target document
            auto filter = bsoncxx::builder::stream::document{} << "_id" << id << bsoncxx::builder::stream::finalize;
            trace("filter doc: {}", bsoncxx::to_json(filter));

            // Find the document with the matching _id field
            bsoncxx::stdx::optional<bsoncxx::document::value> result = collection.find_one(filter.view());

            if (!result) {
                info("no creature with ID '{}' found", id.to_string());
                throw creatures::NotFoundException(fmt::format("no creature id '{}' found", id.to_string()));
            }

            // Unwrap the optional to obtain the bsoncxx::document::value
            bsoncxx::document::value found_document = *result;
            creatureFromBson(found_document, creature);

            debug("get completed!");

        }
        catch (const mongocxx::exception &e) {
            critical("an unhandled error happened while loading a creature by ID: {}", e.what());
            throw InternalError(
                    fmt::format("an unhandled error happened while loading a creature by ID: {}", e.what()));
        }
    }

}