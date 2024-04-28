
#include "server/config.h"

#include <string>

#include "spdlog/spdlog.h"

#include "exception/exception.h"
#include "model/Creature.h"

#include "server/database.h"
#include "server/creature-server.h"
#include "util/helpers.h"


#include <fmt/format.h>


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

//    Status CreatureServerImpl::GetCreature(ServerContext *context, const CreatureId *id, server::Creature *reply) {
//        debug("calling gRPCgetCreature()");
//
//        grpc::Status status;
//        try {
//            db->gRPCgetCreature(id, reply);
//            debug("creature found in DB on get!");
//            return {grpc::StatusCode::OK, fmt::format("✅ Got creature by id successfully!")};
//
//        }
//        catch (const creatures::NotFoundException &e) {
//            info("creature id not found");
//            return {grpc::StatusCode::NOT_FOUND, e.what(), "🚫 Creature id not found"};
//
//        }
//        catch (const creatures::DataFormatException &e) {
//            critical("Data format exception while getting a creature by id: {}", e.what());
//            return {grpc::StatusCode::INTERNAL,
//                      e.what(),
//                      "A data formatting error occurred while looking for a creature by id"};
//        }
//        catch (const creatures::InvalidArgumentException &e) {
//            error("an empty name was passed into gRPCgetCreature()");
//            return {grpc::StatusCode::INVALID_ARGUMENT,
//                      e.what(),
//                      fmt::format("⚠️ A creature id must be supplied")};
//
//        }
//    }



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
//    void Database::gRPCgetCreature(const CreatureId *creatureId, server::Creature *creature) {
//
//        if (creatureId->_id().empty()) {
//            info("an empty creatureID was passed into gRPCgetCreature()");
//            throw InvalidArgumentException("unable to get a creature because the id was empty");
//        }
//
//        // Convert the ID into MongoID's ID
//        bsoncxx::oid id = bsoncxx::oid(creatureId->_id().data(), bsoncxx::oid::k_oid_length);
//        debug("attempting to search for a creature by ID: {}", id.to_string());
//
//        auto collection = getCollection(CREATURES_COLLECTION);
//        trace("collection located");
//
//        try {
//
//            // Create a filter BSON document to match the target document
//            auto filter = bsoncxx::builder::stream::document{} << "_id" << id << bsoncxx::builder::stream::finalize;
//            trace("filter doc: {}", bsoncxx::to_json(filter));
//
//            // Find the document with the matching _id field
//            bsoncxx::stdx::optional<bsoncxx::document::value> result = collection.find_one(filter.view());
//
//            if (!result) {
//                info("no creature with ID '{}' found", id.to_string());
//                throw creatures::NotFoundException(fmt::format("no creature id '{}' found", id.to_string()));
//            }
//
//            // Unwrap the optional to obtain the bsoncxx::document::value
//            bsoncxx::document::value found_document = *result;
//            gRPCCreatureFromBson(found_document, creature);
//
//            debug("get completed!");
//
//        }
//        catch (const mongocxx::exception &e) {
//            critical("an unhandled error happened while loading a creature by ID: {}", e.what());
//            throw InternalError(
//                    fmt::format("an unhandled error happened while loading a creature by ID: {}", e.what()));
//        }
//    }

    /**
     * Get a creature from the database
     *
     * @param creatureId The creature ID to look up
     *
     * @throws InvalidArgumentException if creatureID is empty
     * @throws CreatureNotFoundException if the creature ID is not found
     * @throws InternalError if a database error occurs
     *
     */
    creatures::Creature Database::getCreature(std::string creatureId) {

        if (creatureId.empty()) {
            info("an empty creatureID was passed into gRPCgetCreature()");
            throw creatures::InvalidArgumentException("unable to get a creature because the id was empty");
        }

        Creature c;

        // Convert the ID into MongoID's oid
        bsoncxx::oid id = stringToOid(creatureId);
        debug("attempting to search for a creature by ID: {}", creatureId);

        auto collection = getCollection(CREATURES_COLLECTION);
        trace("collection located");

        try {

            // Create a filter BSON document to match the target document
            auto filter = bsoncxx::builder::stream::document{} << "_id" << id << bsoncxx::builder::stream::finalize;
            trace("filter doc: {}", bsoncxx::to_json(filter));

            // Find the document with the matching _id field
            bsoncxx::stdx::optional<bsoncxx::document::value> result = collection.find_one(filter.view());

            if (!result) {
                info("no creature with ID '{}' found", creatureId);
                throw creatures::NotFoundException(fmt::format("no creature id '{}' found", creatureId));
            }

            // Unwrap the optional to obtain the bsoncxx::document::value
            bsoncxx::document::value found_document = *result;
            c = creatureFromBson(found_document);

            debug("get completed!");
            return c;

        }
        catch (const mongocxx::exception &e) {
            std::string errorMessage = fmt::format("an unhandled error happened while loading a creature by ID: {}", e.what());
            critical(errorMessage);
            throw creatures::InternalError(errorMessage);
        }
    }



}