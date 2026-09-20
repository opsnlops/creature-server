
#include "server/config.h"

#include <spdlog/spdlog.h>
#include <string>

#include <nlohmann/json.hpp>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>

#include "model/Stage.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/Result.cpp"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

// All paths through this file follow the canonical Database observability
// pattern documented in docs/database-observability.md (issue #17).

Result<creatures::Stage> Database::upsertStage(const std::string &stageJson,
                                               const std::shared_ptr<OperationSpan> &parentSpan) {

    if (!parentSpan) {
        warn("no parent span provided for Database.upsertStage, creating a root span");
    }
    auto upsertSpan = creatures::observability->createChildOperationSpan("Database.upsertStage", parentSpan);

    if (upsertSpan) {
        upsertSpan->setAttribute("database.collection", STAGES_COLLECTION);
        upsertSpan->setAttribute("database.operation", "replace_one");
        upsertSpan->setAttribute("database.system", "mongodb");
        upsertSpan->setAttribute("database.name", DB_NAME);
    }

    info("attempting to upsert a Stage in the database");
    try {

        auto parseJsonSpan = creatures::observability->createChildOperationSpan("upsertStage.parse-json", upsertSpan);
        auto jsonResult = JsonParser::parseJsonString(stageJson, "stage upsert", parseJsonSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            recordSpanError(upsertSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<Stage>{err};
        }
        auto jsonObject = jsonResult.getValue().value();

        auto stageResult = stageFromJson(jsonObject, upsertSpan);
        if (!stageResult.isSuccess()) {
            auto err = stageResult.getError().value();
            auto errorMessage = fmt::format("Error while creating a Stage from JSON: {}", err.getMessage());
            recordSpanError(upsertSpan, errorMessage, "InvalidData", err.getCode());
            warn(errorMessage);
            return Result<Stage>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        auto stage = stageResult.getValue().value();
        if (upsertSpan) {
            // Set stage.id early so error paths in later stages carry it too.
            upsertSpan->setAttribute("stage.id", stage.id);
        }

        // jsonStringToBson is the load-bearing opaque-preservation step: the
        // *original* JSON string is converted to BSON, so console-owned keys
        // the server doesn't model — per-placement gain/muted/display colour,
        // and everything inside `audio` — land in MongoDB untouched.
        // stageFromJson above validates the geometry we DO care about; this
        // step preserves everything else, which is what keeps this one
        // document instead of two that drift apart.
        auto bsonSpan = creatures::observability->createChildOperationSpan("upsertStage.json-to-bson", upsertSpan);
        auto bsonResult = JsonParser::jsonStringToBson(stageJson, fmt::format("stage {}", stage.id), bsonSpan);
        if (!bsonResult.isSuccess()) {
            auto err = bsonResult.getError().value();
            recordSpanError(upsertSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<Stage>{err};
        }
        auto bsonDoc = bsonResult.getValue().value();

        auto collectionSpan =
            creatures::observability->createChildOperationSpan("upsertStage.get-collection", upsertSpan);
        auto collectionResult = getCollection(STAGES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage =
                fmt::format("database error while getting stages collection: {}", err.getMessage());
            if (collectionSpan) {
                collectionSpan->setError(errorMessage);
                collectionSpan->setAttribute("error.type", "DatabaseError");
                collectionSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
            }
            recordSpanError(upsertSpan, errorMessage, "DatabaseError", err.getCode());
            warn(errorMessage);
            return Result<Stage>{err};
        }
        auto collectionLease = collectionResult.getValue().value();
        auto &collection = collectionLease->collection();
        if (collectionSpan)
            collectionSpan->setSuccess();

        auto mongoSpan = creatures::observability->createChildOperationSpan("upsertStage.mongoQuery", upsertSpan);
        auto id = stage.id;
        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << id;

        // REPLACE, not $set (#135). A $set upsert cannot remove a field, so no
        // caller can ever delete one — the failure is silent and returns 200.
        // See #134, where clearing an accepted voice take did exactly that.
        // The document handed to this function IS the stored document.
        mongocxx::options::replace replace_options;
        replace_options.upsert(true);

        collection.replace_one(filter_builder.view(), bsonDoc.view(), replace_options);
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (upsertSpan) {
            upsertSpan->setAttribute("stage.title", stage.title);
            upsertSpan->setAttribute("stage.placements_count", static_cast<int64_t>(stage.placements.size()));
            upsertSpan->setAttribute("stage.notes_length", static_cast<int64_t>(stage.notes.size()));
            upsertSpan->setSuccess();
        }

        info("Stage upserted in the database: {}", stage.id);
        return Result<Stage>{stage};

    } catch (const mongocxx::exception &e) {
        auto errorMessage =
            fmt::format("Error (mongocxx::exception) while upserting a stage in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan) {
            upsertSpan->recordException(e);
        }
        recordSpanError(upsertSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<Stage>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const bsoncxx::exception &e) {
        auto errorMessage = fmt::format("Error (bsoncxx::exception) while upserting a stage in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan) {
            upsertSpan->recordException(e);
        }
        recordSpanError(upsertSpan, errorMessage, "JsonParsingException", ServerError::InvalidData);
        return Result<Stage>{ServerError(ServerError::InvalidData, errorMessage)};
    } catch (...) {
        std::string errorMessage = "Unknown error while adding a stage to the database";
        critical(errorMessage);
        recordSpanError(upsertSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<Stage>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<void> Database::deleteStage(const stageId_t &stageId, const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.deleteStage, creating a root span");
    }
    auto span = creatures::observability->createChildOperationSpan("Database.deleteStage", parentSpan);
    if (span) {
        span->setAttribute("database.collection", STAGES_COLLECTION);
        span->setAttribute("database.operation", "delete_one");
        span->setAttribute("database.system", "mongodb");
        span->setAttribute("database.name", DB_NAME);
        span->setAttribute("stage.id", stageId);
    }

    if (stageId.empty()) {
        std::string errorMessage = "deleteStage called with empty stageId";
        warn(errorMessage);
        recordSpanError(span, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<void>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    try {
        auto collectionResult = getCollection(STAGES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(span, err.getMessage(), "DatabaseError", err.getCode());
            return Result<void>{err};
        }
        auto collectionLease = collectionResult.getValue().value();
        auto &collection = collectionLease->collection();

        auto mongoSpan = creatures::observability->createChildOperationSpan("deleteStage.mongoQuery", span);
        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << stageId;

        auto result = collection.delete_one(filter_builder.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!result || result->deleted_count() == 0) {
            std::string errorMessage = fmt::format("Stage {} not found while deleting", stageId);
            warn(errorMessage);
            recordSpanError(span, errorMessage, "NotFound", ServerError::NotFound);
            return Result<void>{ServerError(ServerError::NotFound, errorMessage)};
        }

        if (span)
            span->setSuccess();
        return Result<void>{};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage = fmt::format("Error while deleting stage {}: {}", stageId, e.what());
        error(errorMessage);
        if (span) {
            span->recordException(e);
        }
        recordSpanError(span, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown error while deleting stage {}", stageId);
        critical(errorMessage);
        recordSpanError(span, errorMessage, "std::exception", ServerError::InternalError);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures
