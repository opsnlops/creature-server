
#include "server/config.h"

#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fmt/format.h>


#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <bsoncxx/builder/stream/document.hpp>


#include "exception/exception.h"
#include "model/Creature.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "util/cache.h"
#include "util/helpers.h"
#include "util/Result.h"


#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;
using json = nlohmann::json;

namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;



    Result<json> Database::getCreatureJson(creatureId_t creatureId) {

        debug("attempting to get a creature's JSON by ID: {}", creatureId);

        if(creatureId.empty()) {
            info("an empty creatureID was passed into getCreatureJson()");
            return Result<json>{ServerError(ServerError::InvalidData, "unable to get a creature because the id was empty")};
        }

        try {
            bsoncxx::builder::stream::document filter_builder;
            filter_builder << "id" << creatureId;

            // Search for the document
           auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
            if(!collectionResult.isSuccess()) {
                auto error = collectionResult.getError().value();
                std::string errorMessage = fmt::format("database error while getting a creature's JSON: {}", error.getMessage());
                warn(errorMessage);
                return Result<json>{error};
            }
            auto collection = collectionResult.getValue().value();
            auto maybe_result = collection.find_one(filter_builder.view());

            // Check if the document was found
            if (maybe_result) {
                // Convert BSON document to JSON using nlohmann::json
                bsoncxx::document::view view = maybe_result->view();
                nlohmann::json json_result = nlohmann::json::parse(bsoncxx::to_json(view));
                return Result<json>{json_result};
            } else {
                std::string errorMessage = fmt::format("no creature id '{}' found", creatureId);
                warn(errorMessage);
                return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
            }
        } catch (const mongocxx::exception &e) {
            std::string errorMessage = fmt::format("a MongoDB error happened while loading a creature by ID: {}", e.what());
            critical(errorMessage);
            return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
        } catch ( ... ) {
            std::string errorMessage = fmt::format("An unknown error happened while loading a creature by ID");
            critical(errorMessage);
            return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
        }

    }



    /**
     * Get a creature from the database
     *
     * @param creatureId The creature ID to look up
     *
     */
    Result<creatures::Creature> Database::getCreature(const creatureId_t& creatureId) {

        if (creatureId.empty()) {
            std::string errorMessage = "unable to get a creature because the id was empty";
            warn(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }

        // Go to the database and get the creature
        auto creatureJson = getCreatureJson(creatureId);
        if(!creatureJson.isSuccess()) {
            auto error = creatureJson.getError().value();
            std::string errorMessage = fmt::format("unable to get a creature by ID: {}", creatureJson.getError()->getMessage());
            warn(errorMessage);
            return Result<creatures::Creature>{error};
        }

        // Covert it to our Creature object if we can
        auto result = creatureFromJson(creatureJson.getValue().value());
        if(!result.isSuccess()) {
            auto error = result.getError().value();
            std::string errorMessage = fmt::format("unable to get a creature by ID: {}", result.getError()->getMessage());
            warn(errorMessage);
            return Result<creatures::Creature>{error};
        }

        // Create the creature
        auto creature = result.getValue().value();

        // As long as we're here, let's update the cache
        creatureCache->put(creatureId, creature);
        return Result<creatures::Creature>{creature};
    }



}