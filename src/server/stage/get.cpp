
#include "server/config.h"

#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>

#include "model/Stage.h"
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

Result<creatures::Stage> Database::getStage(const stageId_t &stageId,
                                            const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getStage, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getStage", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", STAGES_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("stage.id", stageId);
    }

    if (stageId.empty()) {
        std::string errorMessage = "unable to get a stage because the id was empty";
        warn(errorMessage);
        recordSpanError(dbSpan, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<Stage>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto collectionResult = getCollection(STAGES_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        std::string errorMessage = fmt::format("unable to get the stages collection: {}", err.getMessage());
        critical(errorMessage);
        recordSpanError(dbSpan, errorMessage, "DatabaseError", err.getCode());
        return Result<Stage>{err};
    }
    auto collection = collectionResult.getValue().value();

    std::shared_ptr<OperationSpan> mongoSpan;
    try {
        mongoSpan = creatures::observability->createChildOperationSpan("getStage.mongoQuery", dbSpan);

        auto query = document{} << "id" << stageId << finalize;
        auto maybe_result = collection.find_one(query.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!maybe_result) {
            std::string errorMessage = fmt::format("Stage not found: {}", stageId);
            warn(errorMessage);
            recordSpanError(dbSpan, errorMessage, "NotFound", ServerError::NotFound);
            return Result<Stage>{ServerError(ServerError::NotFound, errorMessage)};
        }

        auto convertSpan = creatures::observability->createChildOperationSpan("getStage.bson-to-json", dbSpan);
        auto jsonResult = JsonParser::bsonToJson(maybe_result->view(), fmt::format("stage {}", stageId), convertSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            warn("Failed to convert BSON to JSON for stage ID: {} - {}", stageId, err.getMessage());
            recordSpanError(dbSpan, err.getMessage(), "JsonParsingException", err.getCode());
            return Result<Stage>{err};
        }

        auto fetchSpan = creatures::observability->createChildOperationSpan("getStage.stageFromJson", dbSpan);
        auto result = stageFromJson(jsonResult.getValue().value(), fetchSpan);
        if (!result.isSuccess()) {
            auto err = result.getError().value();
            std::string errorMessage = fmt::format("unable to parse stage {}: {}", stageId, err.getMessage());
            warn(errorMessage);
            recordSpanError(dbSpan, errorMessage, "InvalidData", err.getCode());
            return Result<Stage>{err};
        }
        if (fetchSpan)
            fetchSpan->setSuccess();

        if (dbSpan)
            dbSpan->setSuccess();
        return Result<Stage>{result.getValue().value()};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage =
            fmt::format("MongoDB exception caught while finding stage {}: {}", stageId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
        }
        if (dbSpan) {
            dbSpan->recordException(e);
        }
        recordSpanError(dbSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<Stage>{ServerError(ServerError::DatabaseError, errorMessage)};
    } catch (const std::exception &e) {
        std::string errorMessage =
            fmt::format("Standard exception caught while finding stage {}: {}", stageId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
        }
        if (dbSpan) {
            dbSpan->recordException(e);
        }
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<Stage>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown exception caught while finding stage {}", stageId);
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->setError(errorMessage);
        }
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<Stage>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<std::vector<creatures::Stage>> Database::listStages(const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.listStages, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.listStages", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", STAGES_COLLECTION);
        dbSpan->setAttribute("database.operation", "find");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
    }

    info("attempting to list all Stages");

    auto stageList = std::vector<Stage>{};

    try {
        document query_doc{};
        document sort_doc{};
        // Newest-first so the Console's stage list defaults to most-recently-edited.
        sort_doc << "updated_at" << -1;

        auto collectionResult = getCollection(STAGES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage = fmt::format("unable to get the stages collection: {}", err.getMessage());
            critical(errorMessage);
            recordSpanError(dbSpan, errorMessage, "DatabaseError", err.getCode());
            return Result<std::vector<Stage>>{err};
        }
        auto collection = collectionResult.getValue().value();

        auto mongoSpan = creatures::observability->createChildOperationSpan("listStages.mongoQuery", dbSpan);
        mongocxx::options::find opts;
        opts.sort(sort_doc.view());
        mongocxx::cursor cursor = collection.find(query_doc.view(), opts);

        for (auto doc : cursor) {
            auto stageSpan = creatures::observability->createChildOperationSpan("listStages.create-stage", mongoSpan);

            auto jsonResult = JsonParser::bsonToJson(doc, "stage document", stageSpan);
            if (!jsonResult.isSuccess()) {
                auto err = jsonResult.getError().value();
                if (stageSpan) {
                    stageSpan->setError(err.getMessage());
                    stageSpan->setAttribute("error.type", "JsonParsingException");
                    stageSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
                }
                continue;
            }
            json j = jsonResult.getValue().value();

            auto result = stageFromJson(j, stageSpan);
            if (!result.isSuccess()) {
                auto err = result.getError().value();
                std::string errorMessage = fmt::format("Data format error while listing stages: {}", err.getMessage());
                critical(errorMessage);
                if (stageSpan) {
                    stageSpan->setError(errorMessage);
                    stageSpan->setAttribute("error.type", "DataFormatException");
                    stageSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
                }
                recordSpanError(dbSpan, errorMessage, "DataFormatException", err.getCode());
                return Result<std::vector<Stage>>{err};
            }
            stageList.push_back(result.getValue().value());

            if (stageSpan) {
                stageSpan->setAttribute("stage.id", result.getValue().value().id);
                stageSpan->setSuccess();
            }
        }
        if (mongoSpan) {
            mongoSpan->setAttribute("stages.count", static_cast<int64_t>(stageList.size()));
            mongoSpan->setSuccess();
        }

        debug("found {} stages", stageList.size());
        if (dbSpan) {
            dbSpan->setAttribute("stages.count", static_cast<int64_t>(stageList.size()));
            dbSpan->setSuccess();
        }
        return Result<std::vector<Stage>>{stageList};

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Failed to list stages: {}", e.what());
        error(errorMessage);
        if (dbSpan) {
            dbSpan->recordException(e);
        }
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<std::vector<Stage>>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = "Failed to list stages: unknown error";
        error(errorMessage);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<std::vector<Stage>>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures
