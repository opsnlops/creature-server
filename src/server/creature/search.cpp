
#include "server/config.h"

#include <string>

#include "spdlog/spdlog.h"

#include "exception/exception.h"
#include "server/database.h"
#include "server/creature-server.h"


#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>



#include <bsoncxx/builder/stream/document.hpp>

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;

//    Status CreatureServerImpl::SearchCreatures(ServerContext *context, const CreatureName *request, server::Creature *reply) {
//        debug("calling searching for a creature");
//
//        grpc::Status status;
//
//        try {
//            db->searchCreatures(request, reply);
//            debug("creature {} found in DB!", request->name());
//            return {grpc::StatusCode::OK,
//                      fmt::format("✅ Searched for creature name '{}' successfully!", request->name())};
//
//        }
//        catch (const creatures::NotFoundException &e) {
//            std::string errorMessage = fmt::format("creature {} not found", request->name());
//            info(errorMessage);
//            return {grpc::StatusCode::NOT_FOUND, e.what(), errorMessage};
//
//        }
//        catch (const creatures::DataFormatException &e) {
//            std::string errorMessage = fmt::format("Data format exception while looking for a creature: {}", e.what());
//            critical(errorMessage);
//            return {grpc::StatusCode::INTERNAL, e.what(), errorMessage};
//
//        }
//        catch (const creatures::InvalidArgumentException &e) {
//            std::string errorMessage = "an empty name was passed into searchCreatures()";
//            error(errorMessage);
//            return {grpc::StatusCode::INVALID_ARGUMENT,
//                                  e.what(),
//                                  fmt::format("⚠️ A creature name must be supplied")};
//        }
//
//    }
//
//    void Database::searchCreatures(const CreatureName *creatureName, server::Creature *creature) {
//
//        grpc::Status status;
//        if (creatureName->name().empty()) {
//            info("an attempt to search for an empty name was made");
//            throw InvalidArgumentException("unable to search for Creatures because the name was empty");
//        }
//
//        // Okay, we know we have a non-empty name
//        std::string name = creatureName->name();
//        debug("attempting to search for a creature named {}", name);
//
//        auto collection = getCollection(CREATURES_COLLECTION);
//        trace("collection located");
//
//        try {
//            // Create a filter BSON document to match the target document
//            auto filter = bsoncxx::builder::stream::document{} << "name" << name << bsoncxx::builder::stream::finalize;
//
//            // Find the document with the matching _id field
//            bsoncxx::stdx::optional<bsoncxx::document::value> result = collection.find_one(filter.view());
//
//            if (!result) {
//                info("no creatures named '{}' found", name);
//                throw creatures::NotFoundException(fmt::format("no creatures named '{}' found", name));
//            }
//
//            // Unwrap the optional to obtain the bsoncxx::document::value
//            bsoncxx::document::value found_document = *result;
//            gRPCCreatureFromBson(found_document, creature);
//
//            debug("find completed!");
//
//        }
//        catch (const mongocxx::exception &e) {
//            critical("an unhandled error happened while searching for a creature: {}", e.what());
//            throw InternalError(
//                    fmt::format("an unhandled error happened while searching for a creature: {}", e.what()));
//        }
//    }


}