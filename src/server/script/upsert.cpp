
#include "server/config.h"

#include <spdlog/spdlog.h>
#include <string>

#include <nlohmann/json.hpp>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>

#include "model/DialogScript.h"
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

Result<creatures::DialogScript> Database::upsertDialogScript(const std::string &scriptJson,
                                                             const std::shared_ptr<OperationSpan> &parentSpan) {

    if (!parentSpan) {
        warn("no parent span provided for Database.upsertDialogScript, creating a root span");
    }
    auto upsertSpan = creatures::observability->createChildOperationSpan("Database.upsertDialogScript", parentSpan);

    if (upsertSpan) {
        upsertSpan->setAttribute("database.collection", DIALOG_SCRIPTS_COLLECTION);
        upsertSpan->setAttribute("database.operation", "update_one");
        upsertSpan->setAttribute("database.system", "mongodb");
        upsertSpan->setAttribute("database.name", DB_NAME);
    }

    info("attempting to upsert a DialogScript in the database");
    try {

        auto parseJsonSpan =
            creatures::observability->createChildOperationSpan("upsertDialogScript::parse-json", upsertSpan);
        auto jsonResult = JsonParser::parseJsonString(scriptJson, "dialog script upsert", parseJsonSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            upsertSpan->setError(err.getMessage());
            return Result<DialogScript>{err};
        }
        auto jsonObject = jsonResult.getValue().value();

        auto scriptResult = dialogScriptFromJson(jsonObject, upsertSpan);
        if (!scriptResult.isSuccess()) {
            auto err = scriptResult.getError().value();
            auto errorMessage = fmt::format("Error while creating a DialogScript from JSON: {}", err.getMessage());
            upsertSpan->setError(errorMessage);
            upsertSpan->setAttribute("error.type", "InvalidData");
            upsertSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
            warn(errorMessage);
            return Result<DialogScript>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        auto script = scriptResult.getValue().value();

        auto bsonSpan =
            creatures::observability->createChildOperationSpan("upsertDialogScript::json-to-bson", upsertSpan);
        auto bsonResult =
            JsonParser::jsonStringToBson(scriptJson, fmt::format("dialog script {}", script.id), bsonSpan);
        if (!bsonResult.isSuccess()) {
            auto err = bsonResult.getError().value();
            upsertSpan->setError(err.getMessage());
            return Result<DialogScript>{err};
        }
        auto bsonDoc = bsonResult.getValue().value();

        auto collectionSpan =
            creatures::observability->createChildOperationSpan("upsertDialogScript::get-collection", upsertSpan);
        auto collectionResult = getCollection(DIALOG_SCRIPTS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage =
                fmt::format("database error while getting dialog script collection: {}", err.getMessage());
            collectionSpan->setError(errorMessage);
            warn(errorMessage);
            return Result<DialogScript>{err};
        }
        auto collection = collectionResult.getValue().value();
        collectionSpan->setSuccess();

        auto mongoSpan = creatures::observability->createChildOperationSpan("upsertDialogScript::mongo", upsertSpan);
        auto id = script.id;
        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << id;

        mongocxx::options::update update_options;
        update_options.upsert(true);

        collection.update_one(filter_builder.view(),
                              bsoncxx::builder::stream::document{} << "$set" << bsonDoc.view()
                                                                   << bsoncxx::builder::stream::finalize,
                              update_options);
        mongoSpan->setSuccess();

        if (upsertSpan) {
            upsertSpan->setAttribute("script.id", script.id);
            upsertSpan->setAttribute("script.title", script.title);
            upsertSpan->setAttribute("script.turns_count", static_cast<int64_t>(script.turns.size()));
            upsertSpan->setSuccess();
        }

        info("DialogScript upserted in the database: {}", script.id);
        return Result<DialogScript>{script};

    } catch (const mongocxx::exception &e) {
        auto errorMessage =
            fmt::format("Error (mongocxx::exception) while upserting a dialog script in database: {}", e.what());
        error(errorMessage);
        upsertSpan->recordException(e);
        upsertSpan->setError(errorMessage);
        return Result<DialogScript>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const bsoncxx::exception &e) {
        auto errorMessage =
            fmt::format("Error (bsoncxx::exception) while upserting a dialog script in database: {}", e.what());
        upsertSpan->recordException(e);
        upsertSpan->setError(errorMessage);
        error(errorMessage);
        return Result<DialogScript>{ServerError(ServerError::InvalidData, errorMessage)};
    } catch (...) {
        std::string errorMessage = "Unknown error while adding a dialog script to the database";
        upsertSpan->setError(errorMessage);
        critical(errorMessage);
        return Result<DialogScript>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<void> Database::deleteDialogScript(const scriptId_t &scriptId,
                                          const std::shared_ptr<OperationSpan> &parentSpan) {
    auto span = creatures::observability->createChildOperationSpan("Database.deleteDialogScript", parentSpan);
    if (span) {
        span->setAttribute("script.id", scriptId);
    }

    if (scriptId.empty()) {
        std::string errorMessage = "deleteDialogScript called with empty scriptId";
        warn(errorMessage);
        if (span)
            span->setError(errorMessage);
        return Result<void>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    try {
        auto collectionResult = getCollection(DIALOG_SCRIPTS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            if (span)
                span->setError(err.getMessage());
            return Result<void>{err};
        }
        auto collection = collectionResult.getValue().value();

        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << scriptId;

        auto result = collection.delete_one(filter_builder.view());
        if (!result || result->deleted_count() == 0) {
            std::string errorMessage = fmt::format("Dialog script {} not found while deleting", scriptId);
            warn(errorMessage);
            if (span)
                span->setError(errorMessage);
            return Result<void>{ServerError(ServerError::NotFound, errorMessage)};
        }

        if (span)
            span->setSuccess();
        return Result<void>{};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage = fmt::format("Error while deleting dialog script {}: {}", scriptId, e.what());
        error(errorMessage);
        if (span) {
            span->recordException(e);
            span->setError(errorMessage);
        }
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown error while deleting dialog script {}", scriptId);
        critical(errorMessage);
        if (span)
            span->setError(errorMessage);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures
