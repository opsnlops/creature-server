
#include "server/config.h"

#include <spdlog/spdlog.h>
#include <string>

#include <nlohmann/json.hpp>

#include <bsoncxx/builder/stream/document.hpp>
#include <mongocxx/client.hpp>

#include <bsoncxx/document/value.hpp> // for document::value
#include <bsoncxx/json.hpp>           // for from_json

#include "model/Creature.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/Result.cpp"
#include "util/cache.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::stream::document;

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
// Conforms to docs/database-observability.md (issue #17).

Result<creatures::Creature> Database::upsertCreature(const std::string &creatureJson,
                                                     const std::shared_ptr<OperationSpan> &parentSpan) {

    if (!parentSpan) {
        warn("no parent span provided for Database.upsertCreature, creating a root span");
    }
    auto upsertSpan = creatures::observability->createChildOperationSpan("Database.upsertCreature", parentSpan);
    if (upsertSpan) {
        upsertSpan->setAttribute("database.collection", CREATURES_COLLECTION);
        upsertSpan->setAttribute("database.operation", "update_one");
        upsertSpan->setAttribute("database.system", "mongodb");
        upsertSpan->setAttribute("database.name", DB_NAME);
    }

    info("attempting to upsert a creature in the database");
    try {
        auto parseJsonSpan =
            creatures::observability->createChildOperationSpan("upsertCreature.parse-json", upsertSpan);
        auto jsonResult = JsonParser::parseJsonString(creatureJson, "creature upsert", parseJsonSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            recordSpanError(upsertSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<creatures::Creature>{err};
        }
        auto jsonObject = jsonResult.getValue().value();

        auto result = creatureFromJson(jsonObject, upsertSpan);
        if (!result.isSuccess()) {
            auto err = result.getError().value();
            auto errorMessage = fmt::format("Error while creating a creature from JSON: {}", err.getMessage());
            recordSpanError(upsertSpan, errorMessage, "InvalidData", err.getCode());
            warn(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        auto creature = result.getValue().value();
        if (upsertSpan)
            upsertSpan->setAttribute("creature.id", creature.id);

        auto validateSpan = creatures::observability->createChildOperationSpan("upsertCreature.validate", upsertSpan);
        auto failValidation = [&](const std::string &msg) {
            if (validateSpan) {
                validateSpan->setError(msg);
                validateSpan->setAttribute("error.type", "InvalidData");
                validateSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            }
            recordSpanError(upsertSpan, msg, "InvalidData", ServerError::InvalidData);
            warn(msg);
        };
        if (creature.id.empty()) {
            std::string errorMessage = "Creature id is empty";
            failValidation(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        if (creature.name.empty()) {
            std::string errorMessage = "Creature name is empty";
            failValidation(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        if (creature.channel_offset > 511) {
            std::string errorMessage = "Creature channel_offset is greater than 511";
            failValidation(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        if (validateSpan)
            validateSpan->setSuccess();

        auto bsonSpan = creatures::observability->createChildOperationSpan("upsertCreature.json-to-bson", upsertSpan);
        auto bsonResult = JsonParser::jsonStringToBson(creatureJson, fmt::format("creature {}", creature.id), bsonSpan);
        if (!bsonResult.isSuccess()) {
            auto err = bsonResult.getError().value();
            recordSpanError(upsertSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<creatures::Creature>{err};
        }
        auto bsonDoc = bsonResult.getValue().value();

        auto collectionSpan =
            creatures::observability->createChildOperationSpan("upsertCreature.get-collection", upsertSpan);
        auto collectionResult = getCollection(CREATURES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage = fmt::format("database error while getting creature: {}", err.getMessage());
            if (collectionSpan) {
                collectionSpan->setError(errorMessage);
                collectionSpan->setAttribute("error.type", "DatabaseError");
                collectionSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
            }
            recordSpanError(upsertSpan, errorMessage, "DatabaseError", err.getCode());
            warn(errorMessage);
            return Result<creatures::Creature>{err};
        }
        auto collection = collectionResult.getValue().value();
        if (collectionSpan)
            collectionSpan->setSuccess();

        auto mongoSpan = creatures::observability->createChildOperationSpan("upsertCreature.mongoQuery", upsertSpan);
        auto id = creature.id;
        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << id;

        mongocxx::options::update update_options;
        update_options.upsert(true);

        collection.update_one(filter_builder.view(),
                              bsoncxx::builder::stream::document{} << "$set" << bsonDoc.view()
                                                                   << bsoncxx::builder::stream::finalize,
                              update_options);
        if (mongoSpan)
            mongoSpan->setSuccess();

        creatureCache->put(creature.id, creature);

        info("Creature upserted in the database: {}", creature.id);
        if (upsertSpan) {
            upsertSpan->setAttribute("creature.name", creature.name);
            upsertSpan->setSuccess();
        }
        return Result<creatures::Creature>{creature};

    } catch (const mongocxx::exception &e) {
        auto errorMessage =
            fmt::format("Error (mongocxx::exception) while upserting a creature in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan)
            upsertSpan->recordException(e);
        recordSpanError(upsertSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<creatures::Creature>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const bsoncxx::exception &e) {
        auto errorMessage =
            fmt::format("Error (bsoncxx::exception) while upserting a creature in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan)
            upsertSpan->recordException(e);
        recordSpanError(upsertSpan, errorMessage, "JsonParsingException", ServerError::InvalidData);
        return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
    } catch (...) {
        std::string errorMessage = "Unknown error while adding a creature to the database";
        critical(errorMessage);
        recordSpanError(upsertSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<creatures::Creature>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures