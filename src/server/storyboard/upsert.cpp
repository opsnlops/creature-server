
#include "server/config.h"

#include <spdlog/spdlog.h>
#include <string>

#include <nlohmann/json.hpp>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>

#include "model/Storyboard.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/Result.cpp"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

// All paths through this file follow the canonical Database observability
// pattern documented in docs/database-observability.md (issue #17).

Result<creatures::Storyboard> Database::upsertStoryboard(const std::string &storyboardJson,
                                                         const std::shared_ptr<OperationSpan> &parentSpan) {

    if (!parentSpan) {
        warn("no parent span provided for Database.upsertStoryboard, creating a root span");
    }
    auto upsertSpan = creatures::observability->createChildOperationSpan("Database.upsertStoryboard", parentSpan);

    if (upsertSpan) {
        upsertSpan->setAttribute("database.collection", STORYBOARDS_COLLECTION);
        upsertSpan->setAttribute("database.operation", "update_one");
        upsertSpan->setAttribute("database.system", "mongodb");
        upsertSpan->setAttribute("database.name", DB_NAME);
    }

    auto setSpanError = [&](const std::string &msg, const std::string &type, ServerError::Code code) {
        if (upsertSpan) {
            upsertSpan->setError(msg);
            upsertSpan->setAttribute("error.type", type);
            upsertSpan->setAttribute("error.code", static_cast<int64_t>(code));
        }
    };

    info("attempting to upsert a Storyboard in the database");
    try {

        auto parseJsonSpan =
            creatures::observability->createChildOperationSpan("upsertStoryboard.parse-json", upsertSpan);
        auto jsonResult = JsonParser::parseJsonString(storyboardJson, "storyboard upsert", parseJsonSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            setSpanError(err.getMessage(), "InvalidData", err.getCode());
            return Result<Storyboard>{err};
        }
        auto jsonObject = jsonResult.getValue().value();

        auto storyboardResult = storyboardFromJson(jsonObject, upsertSpan);
        if (!storyboardResult.isSuccess()) {
            auto err = storyboardResult.getError().value();
            auto errorMessage = fmt::format("Error while creating a Storyboard from JSON: {}", err.getMessage());
            setSpanError(errorMessage, "InvalidData", err.getCode());
            warn(errorMessage);
            return Result<Storyboard>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        auto storyboard = storyboardResult.getValue().value();
        if (upsertSpan) {
            // Set storyboard.id early so error paths in later stages carry it too.
            upsertSpan->setAttribute("storyboard.id", storyboard.id);
        }

        // jsonStringToBson is the load-bearing opaque-preservation step: the
        // *original* JSON string is converted to BSON, so any unknown tile-
        // action keys land in MongoDB untouched. storyboardFromJson above
        // validates known shape; this step preserves everything else.
        auto bsonSpan = creatures::observability->createChildOperationSpan("upsertStoryboard.json-to-bson", upsertSpan);
        auto bsonResult =
            JsonParser::jsonStringToBson(storyboardJson, fmt::format("storyboard {}", storyboard.id), bsonSpan);
        if (!bsonResult.isSuccess()) {
            auto err = bsonResult.getError().value();
            setSpanError(err.getMessage(), "InvalidData", err.getCode());
            return Result<Storyboard>{err};
        }
        auto bsonDoc = bsonResult.getValue().value();

        auto collectionSpan =
            creatures::observability->createChildOperationSpan("upsertStoryboard.get-collection", upsertSpan);
        auto collectionResult = getCollection(STORYBOARDS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage =
                fmt::format("database error while getting storyboards collection: {}", err.getMessage());
            if (collectionSpan) {
                collectionSpan->setError(errorMessage);
                collectionSpan->setAttribute("error.type", "DatabaseError");
                collectionSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
            }
            setSpanError(errorMessage, "DatabaseError", err.getCode());
            warn(errorMessage);
            return Result<Storyboard>{err};
        }
        auto collection = collectionResult.getValue().value();
        if (collectionSpan)
            collectionSpan->setSuccess();

        auto mongoSpan = creatures::observability->createChildOperationSpan("upsertStoryboard.mongoQuery", upsertSpan);
        auto id = storyboard.id;
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

        if (upsertSpan) {
            upsertSpan->setAttribute("storyboard.title", storyboard.title);
            upsertSpan->setAttribute("storyboard.tiles_count", static_cast<int64_t>(storyboard.tiles.size()));
            upsertSpan->setAttribute("storyboard.notes_length", static_cast<int64_t>(storyboard.notes.size()));
            upsertSpan->setSuccess();
        }

        info("Storyboard upserted in the database: {}", storyboard.id);
        return Result<Storyboard>{storyboard};

    } catch (const mongocxx::exception &e) {
        auto errorMessage =
            fmt::format("Error (mongocxx::exception) while upserting a storyboard in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan) {
            upsertSpan->recordException(e);
        }
        setSpanError(errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<Storyboard>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const bsoncxx::exception &e) {
        auto errorMessage =
            fmt::format("Error (bsoncxx::exception) while upserting a storyboard in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan) {
            upsertSpan->recordException(e);
        }
        setSpanError(errorMessage, "JsonParsingException", ServerError::InvalidData);
        return Result<Storyboard>{ServerError(ServerError::InvalidData, errorMessage)};
    } catch (...) {
        std::string errorMessage = "Unknown error while adding a storyboard to the database";
        critical(errorMessage);
        setSpanError(errorMessage, "std::exception", ServerError::InternalError);
        return Result<Storyboard>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<void> Database::deleteStoryboard(const storyboardId_t &storyboardId,
                                        const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.deleteStoryboard, creating a root span");
    }
    auto span = creatures::observability->createChildOperationSpan("Database.deleteStoryboard", parentSpan);
    if (span) {
        span->setAttribute("database.collection", STORYBOARDS_COLLECTION);
        span->setAttribute("database.operation", "delete_one");
        span->setAttribute("database.system", "mongodb");
        span->setAttribute("database.name", DB_NAME);
        span->setAttribute("storyboard.id", storyboardId);
    }

    auto setSpanError = [&](const std::string &msg, const std::string &type, ServerError::Code code) {
        if (span) {
            span->setError(msg);
            span->setAttribute("error.type", type);
            span->setAttribute("error.code", static_cast<int64_t>(code));
        }
    };

    if (storyboardId.empty()) {
        std::string errorMessage = "deleteStoryboard called with empty storyboardId";
        warn(errorMessage);
        setSpanError(errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<void>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    try {
        auto collectionResult = getCollection(STORYBOARDS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            setSpanError(err.getMessage(), "DatabaseError", err.getCode());
            return Result<void>{err};
        }
        auto collection = collectionResult.getValue().value();

        auto mongoSpan = creatures::observability->createChildOperationSpan("deleteStoryboard.mongoQuery", span);
        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << storyboardId;

        auto result = collection.delete_one(filter_builder.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!result || result->deleted_count() == 0) {
            std::string errorMessage = fmt::format("Storyboard {} not found while deleting", storyboardId);
            warn(errorMessage);
            setSpanError(errorMessage, "NotFound", ServerError::NotFound);
            return Result<void>{ServerError(ServerError::NotFound, errorMessage)};
        }

        if (span)
            span->setSuccess();
        return Result<void>{};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage = fmt::format("Error while deleting storyboard {}: {}", storyboardId, e.what());
        error(errorMessage);
        if (span) {
            span->recordException(e);
        }
        setSpanError(errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown error while deleting storyboard {}", storyboardId);
        critical(errorMessage);
        setSpanError(errorMessage, "std::exception", ServerError::InternalError);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures
