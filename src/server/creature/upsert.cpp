
#include "server/config.h"

#include <string>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include <mongocxx/client.hpp>
#include <bsoncxx/builder/stream/document.hpp>

#include <bsoncxx/json.hpp>           // for from_json
#include <bsoncxx/document/value.hpp> // for document::value

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
    extern std::shared_ptr<ObservabilityManager> observability;

    namespace document = bsoncxx::document;

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

         if (!parentSpan) {
            warn("no parent span provided for Database.upsertCreature, creating a root span");
        }
        auto upsertSpan = creatures::observability->createChildOperationSpan("Database.upsertCreature", parentSpan);

        info("attempting to upsert a creature in the database");
        try {

            auto parseJsonSpan = creatures::observability->createChildOperationSpan("upsertCreature::parse-json", upsertSpan);
            auto jsonObject = nlohmann::json::parse(creatureJson);
            parseJsonSpan->setSuccess();

            // Create the Creature object while we're here
            auto result = creatureFromJson(jsonObject, upsertSpan);
            if(!result.isSuccess()) {
                auto error = result.getError();
                auto errorMessage = fmt::format("Error while creating a creature from JSON: {}", error->getMessage());
                upsertSpan->setError(errorMessage);
                upsertSpan->setAttribute("error.type", "InvalidData");
                upsertSpan->setAttribute("error.code", static_cast<int64_t>(error->getCode()));
                warn(errorMessage);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }
            auto creature = result.getValue().value();

            debug("validating creature on upsert");

            auto validateSpan = creatures::observability->createChildOperationSpan("upsertCreature::parse-json", upsertSpan);
            if (creature.id.empty()) {
                std::string errorMessage = "Creature id is empty";
                validateSpan->setError(errorMessage);
                validateSpan->setAttribute("error.type", "InvalidData");
                validateSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                warn(errorMessage);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }

            if (creature.name.empty()) {
                std::string errorMessage = "Creature name is empty";
                validateSpan->setError(errorMessage);
                validateSpan->setAttribute("error.type", "InvalidData");
                validateSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                warn(errorMessage);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }

            if (creature.audio_channel < 0) {
                std::string errorMessage = "Creature audio_channel is less than 0";
                validateSpan->setError(errorMessage);
                validateSpan->setAttribute("error.type", "InvalidData");
                validateSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                warn(errorMessage);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }

            if (creature.channel_offset < 0) {
                std::string errorMessage = "Creature channel_offset is less than 0";
                validateSpan->setError(errorMessage);
                validateSpan->setAttribute("error.type", "InvalidData");
                validateSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                warn(errorMessage);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }

            if (creature.channel_offset > 511) {
                std::string errorMessage = "Creature channel_offset is greater than 511";
                validateSpan->setError(errorMessage);
                validateSpan->setAttribute("error.type", "InvalidData");
                validateSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                warn(errorMessage);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }
            validateSpan->setSuccess();

            // Convert the JSON string into BSON
            auto createBsonDoc = [&]() -> std::optional<document::value> {
                auto bsonSpan = creatures::observability->createChildOperationSpan("upsertCreature::json-to-bson", upsertSpan);
                try {
                    auto doc = bsoncxx::from_json(creatureJson);
                    bsonSpan->setAttribute("json.size_bytes", static_cast<int64_t>(creatureJson.length()));
                    bsonSpan->setSuccess();
                    return doc;
                } catch (const std::exception& e) {
                    bsonSpan->recordException(e);
                    warn("Error while converting JSON to BSON: {}", e.what());
                    return std::nullopt;
                }
            };

            auto bsonDocOpt = createBsonDoc();
            if (!bsonDocOpt) {
                std::string errorMessage = "Failed to convert JSON to BSON";
                upsertSpan->setError(errorMessage);
                upsertSpan->setAttribute("error.type", "InvalidData");
                upsertSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }
            auto bsonDoc = bsonDocOpt.value();

            auto collectionSpan = creatures::observability->createChildOperationSpan("upsertCreature::get-collection", upsertSpan);
            auto collectionResult = getCollection(CREATURES_COLLECTION);
            if(!collectionResult.isSuccess()) {
                auto error = collectionResult.getError().value();
                std::string errorMessage = fmt::format("database error while getting creature: {}", error.getMessage());
                collectionSpan->setError(errorMessage);
                collectionSpan->setAttribute("error.type", "DatabaseError");
                collectionSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::DatabaseError));
                warn(errorMessage);
                return Result<creatures::Creature>{error};
            }
            auto collection = collectionResult.getValue().value();
            trace("collection located");
            collectionSpan->setSuccess();


            auto mongoSpan = creatures::observability->createChildOperationSpan("upsertCreature::mongo", upsertSpan);
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
            mongoSpan->setSuccess();

            // Update the cache now that we know it worked
            creatureCache->put(creature.id, creature);

            info("Creature upserted in the database: {}", creature.id);
            return Result<creatures::Creature>{creature};

        } catch (const mongocxx::exception &e) {
            auto errorMessage = fmt::format("Error (mongocxx::exception) while upserting a creature in database: {}", e.what());
            error(errorMessage);
            upsertSpan->recordException(e);
            upsertSpan->setError(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InternalError, errorMessage)};
        } catch (const bsoncxx::exception &e) {
            auto errorMessage = fmt::format("Error (bsoncxx::exception) while upserting a creature in database: {}", e.what());
            upsertSpan->recordException(e);
            upsertSpan->setError(errorMessage);
            error(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        } catch (...) {
            std::string errorMessage = "Unknown error while adding a creature to the database";
            upsertSpan->setError(errorMessage);
            critical(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InternalError, errorMessage)};
        }

    }

}