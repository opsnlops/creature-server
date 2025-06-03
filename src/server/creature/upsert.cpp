
#include "server/config.h"

#include <string>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include <mongocxx/client.hpp>
#include <bsoncxx/builder/stream/document.hpp>

#include "model/Creature.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "util/cache.h"
#include "util/helpers.h"
#include "util/Result.cpp"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;


    /**
     * Upsert a creature in the database
     *
     * @param creatureJson The full JSON string of the creature. It's stored in the database (as long as all of the
     *                     needed fields are there) so that the controller and console get get a full view of what
     *                     the creature actually is.
     *
     * @return a `Result<creatures::Creature>` the creature that we can return to the client
     */
    Result<creatures::Creature> Database::upsertCreature(const std::string& creatureJson, const std::shared_ptr<OperationSpan>& parentSpan) {

        info("attempting to upsert a creature in the database");

        try {

            auto jsonObject = nlohmann::json::parse(creatureJson);

            // Create the Creature object while we're here
            auto result = creatureFromJson(jsonObject);
            if(!result.isSuccess()) {
                auto error = result.getError();
                warn("Error while creating a creature from JSON: {}", error->getMessage());
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, error->getMessage())};
            }
            auto creature = result.getValue().value();

            debug("validating creature on upsert");

            if (creature.id.empty()) {
                std::string error_message = "Creature id is empty";
                warn(error_message);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, error_message)};
            }

            if (creature.name.empty()) {
                std::string error_message = "Creature name is empty";
                warn(error_message);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, error_message)};
            }

            if (creature.audio_channel < 0) {
                std::string error_message = "Creature audio_channel is less than 0";
                warn(error_message);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, error_message)};
            }

            if (creature.channel_offset < 0) {
                std::string error_message = "Creature channel_offset is less than 0";
                warn(error_message);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, error_message)};
            }

            if (creature.channel_offset > 511) {
                std::string error_message = "Creature channel_offset is greater than 511";
                warn(error_message);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, error_message)};
            }

            // Convert the JSON string into BSON
            auto bsonDoc = bsoncxx::from_json(creatureJson);


            auto collectionResult = getCollection(CREATURES_COLLECTION);
            if(!collectionResult.isSuccess()) {
                auto error = collectionResult.getError().value();
                std::string errorMessage = fmt::format("database error while getting creature: {}", error.getMessage());
                warn(errorMessage);
                return Result<creatures::Creature>{error};
            }
            auto collection = collectionResult.getValue().value();
            trace("collection located");


            auto id = creature.id;
            bsoncxx::builder::stream::document filter_builder;
            filter_builder << "id" << id;

            // Upsert options
            mongocxx::options::update update_options;
            update_options.upsert(true);

            // Upsert operation
            collection.update_one(
                    filter_builder.view(),
                    bsoncxx::builder::stream::document{} << "$set" << bsonDoc.view() << bsoncxx::builder::stream::finalize,
                    update_options
            );

            // Update the cache now that we know it worked
            creatureCache->put(creature.id, creature);

            info("Creature upserted in the database: {}", creature.id);
            return Result<creatures::Creature>{creature};

        } catch (const mongocxx::exception &e) {
            error("Error (mongocxx::exception) while upserting a creature in database: {}", e.what());
            return Result<creatures::Creature>{ServerError(ServerError::InternalError, e.what())};
        } catch (const bsoncxx::exception &e) {
            error("Error (bsoncxx::exception) while upserting a creature in database: {}", e.what());
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, e.what())};
        } catch (...) {
            std::string error_message = "Unknown error while adding a creature to the database";
            critical(error_message);
            return Result<creatures::Creature>{ServerError(ServerError::InternalError, error_message)};
        }

    }

}