
#include "server/config.h"

#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>

#include "model/DialogScript.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::stream::finalize;
using json = nlohmann::json;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

// Conforms to docs/database-observability.md (issue #17).

Result<json> Database::getDialogScriptJson(const scriptId_t &scriptId,
                                           const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getDialogScriptJson, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getDialogScriptJson", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", DIALOG_SCRIPTS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("script.id", scriptId);
    }

    auto setSpanError = [&](const std::string &msg, const std::string &type, ServerError::Code code) {
        if (dbSpan) {
            dbSpan->setError(msg);
            dbSpan->setAttribute("error.type", type);
            dbSpan->setAttribute("error.code", static_cast<int64_t>(code));
        }
    };

    if (scriptId.empty()) {
        std::string errorMessage = "unable to get a dialog script because the id was empty";
        info(errorMessage);
        setSpanError(errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<json>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto collectionResult = getCollection(DIALOG_SCRIPTS_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        std::string errorMessage = fmt::format("unable to get the dialog scripts collection: {}", err.getMessage());
        critical(errorMessage);
        setSpanError(errorMessage, "DatabaseError", err.getCode());
        return Result<json>{err};
    }
    auto collection = collectionResult.getValue().value();

    std::shared_ptr<OperationSpan> mongoSpan;
    try {
        mongoSpan = creatures::observability->createChildOperationSpan("getDialogScriptJson.mongoQuery", dbSpan);

        auto query = document{} << "id" << scriptId << finalize;
        auto maybe_result = collection.find_one(query.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!maybe_result) {
            std::string errorMessage = fmt::format("Dialog script not found: {}", scriptId);
            warn(errorMessage);
            setSpanError(errorMessage, "NotFound", ServerError::NotFound);
            return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
        }

        auto convertSpan =
            creatures::observability->createChildOperationSpan("getDialogScriptJson.bson-to-json", dbSpan);
        auto jsonResult =
            JsonParser::bsonToJson(maybe_result->view(), fmt::format("dialog script {}", scriptId), convertSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            warn("Failed to convert BSON to JSON for dialog script ID: {} - {}", scriptId, err.getMessage());
            setSpanError(err.getMessage(), "JsonParsingException", err.getCode());
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
            fmt::format("MongoDB exception caught while finding dialog script {}: {}", scriptId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
        }
        if (dbSpan) {
            dbSpan->recordException(e);
        }
        setSpanError(errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<json>{ServerError(ServerError::DatabaseError, errorMessage)};
    } catch (const std::exception &e) {
        std::string errorMessage =
            fmt::format("Standard exception caught while finding dialog script {}: {}", scriptId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
        }
        if (dbSpan) {
            dbSpan->recordException(e);
        }
        setSpanError(errorMessage, "std::exception", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown exception caught while finding dialog script {}", scriptId);
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->setError(errorMessage);
        }
        setSpanError(errorMessage, "std::exception", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<creatures::DialogScript> Database::getDialogScript(const scriptId_t &scriptId,
                                                          const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getDialogScript, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getDialogScript", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", DIALOG_SCRIPTS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("script.id", scriptId);
    }

    auto setSpanError = [&](const std::string &msg, const std::string &type, ServerError::Code code) {
        if (dbSpan) {
            dbSpan->setError(msg);
            dbSpan->setAttribute("error.type", type);
            dbSpan->setAttribute("error.code", static_cast<int64_t>(code));
        }
    };

    if (scriptId.empty()) {
        std::string errorMessage = "unable to get a dialog script because the id was empty";
        warn(errorMessage);
        setSpanError(errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<DialogScript>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto jsonSpan = creatures::observability->createChildOperationSpan("getDialogScript.getDialogScriptJson", dbSpan);
    auto scriptJson = getDialogScriptJson(scriptId, jsonSpan);
    if (!scriptJson.isSuccess()) {
        auto err = scriptJson.getError().value();
        std::string errorMessage = fmt::format("unable to get a dialog script by ID: {}", err.getMessage());
        warn(errorMessage);
        // Pick error.type from the underlying code so NotFound vs InvalidData
        // vs DatabaseError stays filterable.
        std::string etype = "InternalError";
        if (err.getCode() == ServerError::NotFound)
            etype = "NotFound";
        else if (err.getCode() == ServerError::InvalidData)
            etype = "InvalidData";
        else if (err.getCode() == ServerError::DatabaseError)
            etype = "DatabaseError";
        setSpanError(errorMessage, etype, err.getCode());
        return Result<DialogScript>{err};
    }
    if (jsonSpan)
        jsonSpan->setSuccess();

    auto fetchSpan = creatures::observability->createChildOperationSpan("getDialogScript.dialogScriptFromJson", dbSpan);
    auto result = dialogScriptFromJson(scriptJson.getValue().value(), fetchSpan);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        std::string errorMessage = fmt::format("unable to get a dialog script by ID: {}", err.getMessage());
        warn(errorMessage);
        setSpanError(errorMessage, "InvalidData", err.getCode());
        return Result<DialogScript>{err};
    }
    if (fetchSpan)
        fetchSpan->setSuccess();

    if (dbSpan) {
        dbSpan->setSuccess();
    }
    return Result<DialogScript>{result.getValue().value()};
}

} // namespace creatures
