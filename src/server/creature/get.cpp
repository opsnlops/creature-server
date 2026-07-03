
#include "server/config.h"

#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>

#include "exception/exception.h"
#include "model/Creature.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h" // Include for ObservabilityManager
#include "util/Result.h"
#include "util/cache.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::stream::document;
using bsoncxx::builder::stream::finalize; // Added for finalize
using json = nlohmann::json;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
extern std::shared_ptr<ObservabilityManager> observability;

// Conforms to docs/database-observability.md (issue #17).

Result<json> Database::getCreatureJson(const creatureId_t &creatureId,
                                       const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getCreatureJson, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getCreatureJson", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", CREATURES_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("creature.id", creatureId);
    }

    debug("attempting to get a creature's JSON by ID: {}", creatureId);

    if (creatureId.empty()) {
        std::string errorMessage = "unable to get a creature because the id was empty";
        info(errorMessage);
        recordSpanError(dbSpan, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<json>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto collectionResult = getCollection(CREATURES_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        std::string errorMessage = fmt::format("unable to get the creature collection: {}", err.getMessage());
        critical(errorMessage);
        recordSpanError(dbSpan, errorMessage, "DatabaseError", err.getCode());
        return Result<json>{err};
    }
    auto collection = collectionResult.getValue().value();

    std::shared_ptr<OperationSpan> mongoSpan;
    try {
        mongoSpan = creatures::observability->createChildOperationSpan("getCreatureJson.mongoQuery", dbSpan);
        auto query = document{} << "id" << creatureId << finalize;
        auto maybe_result = collection.find_one(query.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!maybe_result) {
            std::string errorMessage = fmt::format("Creature not found: {}", creatureId);
            warn(errorMessage);
            recordSpanError(dbSpan, errorMessage, "NotFound", ServerError::NotFound);
            return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
        }

        auto convertSpan = creatures::observability->createChildOperationSpan("getCreatureJson.bson-to-json", dbSpan);
        auto jsonResult =
            JsonParser::bsonToJson(maybe_result->view(), fmt::format("creature {}", creatureId), convertSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            warn("Failed to convert BSON to JSON for creature ID: {} - {}", creatureId, err.getMessage());
            recordSpanError(dbSpan, err.getMessage(), "JsonParsingException", err.getCode());
            return jsonResult;
        }
        json j = jsonResult.getValue().value();

        if (dbSpan) {
            dbSpan->setAttribute("db.response_size_bytes", static_cast<int64_t>(j.dump().length()));
            dbSpan->setSuccess();
        }
        return Result<json>{j};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage =
            fmt::format("MongoDB exception caught while finding creature {}: {}", creatureId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
            mongoSpan->setAttribute("error.type", "MongoDBException");
            mongoSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::DatabaseError));
        }
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<json>{ServerError(ServerError::DatabaseError, errorMessage)};
    } catch (const std::exception &e) {
        std::string errorMessage =
            fmt::format("Standard exception caught while finding creature {}: {}", creatureId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
        }
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown exception caught while finding creature {}", creatureId);
        critical(errorMessage);
        if (mongoSpan)
            mongoSpan->setError(errorMessage);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

/**
 * Look up a creature by it's ID
 *
 * @param creatureId the ID of the creature
 * @return the Creature object that was found, or a ServerError if it couldn't be found or looked up
 *
 */
Result<creatures::Creature> Database::getCreature(const creatureId_t &creatureId,
                                                  const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getCreature, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getCreature", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", CREATURES_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("creature.id", creatureId);
    }

    if (creatureId.empty()) {
        std::string errorMessage = "unable to get a creature because the id was empty";
        warn(errorMessage);
        recordSpanError(dbSpan, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto jsonSpan = creatures::observability->createChildOperationSpan("getCreature.getCreatureJson", dbSpan);
    auto creatureJson = getCreatureJson(creatureId, jsonSpan);
    if (!creatureJson.isSuccess()) {
        auto err = creatureJson.getError().value();
        std::string errorMessage = fmt::format("unable to get a creature by ID: {}", err.getMessage());
        warn(errorMessage);
        std::string etype = "InternalError";
        if (err.getCode() == ServerError::NotFound)
            etype = "NotFound";
        else if (err.getCode() == ServerError::InvalidData)
            etype = "InvalidData";
        else if (err.getCode() == ServerError::DatabaseError)
            etype = "DatabaseError";
        if (jsonSpan) {
            jsonSpan->setError(errorMessage);
            jsonSpan->setAttribute("error.type", etype);
            jsonSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        recordSpanError(dbSpan, errorMessage, etype, err.getCode());
        return Result<creatures::Creature>{err};
    }
    if (jsonSpan)
        jsonSpan->setSuccess();

    auto fetchSpan = creatures::observability->createChildOperationSpan("getCreature.creatureFromJson", dbSpan);
    auto result = creatureFromJson(creatureJson.getValue().value(), fetchSpan);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        std::string errorMessage = fmt::format("unable to get a creature by ID: {}", err.getMessage());
        warn(errorMessage);
        if (fetchSpan) {
            fetchSpan->setError(errorMessage);
            fetchSpan->setAttribute("error.type", "DataFormatException");
            fetchSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        recordSpanError(dbSpan, errorMessage, "DataFormatException", err.getCode());
        return Result<creatures::Creature>{err};
    }
    if (fetchSpan)
        fetchSpan->setSuccess();

    auto creature = result.getValue().value();
    creatureCache->put(creatureId, creature);
    if (dbSpan) {
        dbSpan->setAttribute("creature.name", creature.name);
        dbSpan->setAttribute("cache.status", "updated");
        dbSpan->setSuccess();
    }
    return Result<creatures::Creature>{creature};
}
} // namespace creatures