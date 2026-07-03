
#include "server/config.h"

#include <string>
#include <vector>

#include "exception/exception.h"
#include "model/DialogScript.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"

#include "spdlog/spdlog.h"
#include <fmt/format.h>

#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/find.hpp>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

// Conforms to docs/database-observability.md (issue #17).

Result<std::vector<creatures::DialogScript>>
Database::listDialogScripts(const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.listDialogScripts, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.listDialogScripts", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", DIALOG_SCRIPTS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
    }

    info("attempting to list all DialogScripts");

    auto scriptList = std::vector<DialogScript>{};

    try {
        document query_doc{};
        document sort_doc{};
        // Newest-first so the editor's list defaults to "what was I just working on?"
        sort_doc << "updated_at" << -1;

        auto collectionResult = getCollection(DIALOG_SCRIPTS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage = fmt::format("unable to get the dialog scripts collection: {}", err.getMessage());
            critical(errorMessage);
            recordSpanError(dbSpan, errorMessage, "DatabaseError", err.getCode());
            return Result<std::vector<DialogScript>>{err};
        }
        auto collection = collectionResult.getValue().value();

        auto mongoSpan = creatures::observability->createChildOperationSpan("listDialogScripts.mongoQuery", dbSpan);
        mongocxx::options::find opts;
        opts.sort(sort_doc.view());
        mongocxx::cursor cursor = collection.find(query_doc.view(), opts);

        for (auto doc : cursor) {
            auto scriptSpan =
                creatures::observability->createChildOperationSpan("listDialogScripts.create-script", mongoSpan);

            auto jsonResult = JsonParser::bsonToJson(doc, "dialog script document", scriptSpan);
            if (!jsonResult.isSuccess()) {
                auto err = jsonResult.getError().value();
                if (scriptSpan) {
                    scriptSpan->setError(err.getMessage());
                    scriptSpan->setAttribute("error.type", "JsonParsingException");
                    scriptSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
                }
                continue;
            }
            json j = jsonResult.getValue().value();

            auto result = dialogScriptFromJson(j, scriptSpan);
            if (!result.isSuccess()) {
                auto err = result.getError().value();
                std::string errorMessage =
                    fmt::format("Data format error while listing dialog scripts: {}", err.getMessage());
                critical(errorMessage);
                if (scriptSpan) {
                    scriptSpan->setError(errorMessage);
                    scriptSpan->setAttribute("error.type", "DataFormatException");
                    scriptSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
                }
                recordSpanError(dbSpan, errorMessage, "DataFormatException", err.getCode());
                return Result<std::vector<DialogScript>>{err};
            }
            scriptList.push_back(result.getValue().value());

            if (scriptSpan) {
                scriptSpan->setAttribute("script.id", result.getValue().value().id);
                scriptSpan->setSuccess();
            }
        }
        if (mongoSpan) {
            mongoSpan->setAttribute("scripts.count", static_cast<int64_t>(scriptList.size()));
            mongoSpan->setSuccess();
        }

        debug("found {} dialog scripts", scriptList.size());
        if (dbSpan) {
            dbSpan->setAttribute("scripts.count", static_cast<int64_t>(scriptList.size()));
            dbSpan->setSuccess();
        }
        return Result<std::vector<DialogScript>>{scriptList};

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Failed to list dialog scripts: {}", e.what());
        error(errorMessage);
        if (dbSpan) {
            dbSpan->recordException(e);
        }
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<std::vector<DialogScript>>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = "Failed to list dialog scripts: unknown error";
        error(errorMessage);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<std::vector<DialogScript>>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures
