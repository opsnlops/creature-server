
#include "server/config.h"

#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>

#include "model/Storyboard.h"
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

Result<creatures::Storyboard> Database::getStoryboard(const storyboardId_t &storyboardId,
                                                      const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getStoryboard, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getStoryboard", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", STORYBOARDS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("storyboard.id", storyboardId);
    }

    if (storyboardId.empty()) {
        std::string errorMessage = "unable to get a storyboard because the id was empty";
        warn(errorMessage);
        recordSpanError(dbSpan, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<Storyboard>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto collectionResult = getCollection(STORYBOARDS_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        std::string errorMessage = fmt::format("unable to get the storyboards collection: {}", err.getMessage());
        critical(errorMessage);
        recordSpanError(dbSpan, errorMessage, "DatabaseError", err.getCode());
        return Result<Storyboard>{err};
    }
    auto collection = collectionResult.getValue().value();

    std::shared_ptr<OperationSpan> mongoSpan;
    try {
        mongoSpan = creatures::observability->createChildOperationSpan("getStoryboard.mongoQuery", dbSpan);

        auto query = document{} << "id" << storyboardId << finalize;
        auto maybe_result = collection.find_one(query.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!maybe_result) {
            std::string errorMessage = fmt::format("Storyboard not found: {}", storyboardId);
            warn(errorMessage);
            recordSpanError(dbSpan, errorMessage, "NotFound", ServerError::NotFound);
            return Result<Storyboard>{ServerError(ServerError::NotFound, errorMessage)};
        }

        auto convertSpan = creatures::observability->createChildOperationSpan("getStoryboard.bson-to-json", dbSpan);
        auto jsonResult =
            JsonParser::bsonToJson(maybe_result->view(), fmt::format("storyboard {}", storyboardId), convertSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            warn("Failed to convert BSON to JSON for storyboard ID: {} - {}", storyboardId, err.getMessage());
            recordSpanError(dbSpan, err.getMessage(), "JsonParsingException", err.getCode());
            return Result<Storyboard>{err};
        }

        auto fetchSpan = creatures::observability->createChildOperationSpan("getStoryboard.storyboardFromJson", dbSpan);
        auto result = storyboardFromJson(jsonResult.getValue().value(), fetchSpan);
        if (!result.isSuccess()) {
            auto err = result.getError().value();
            std::string errorMessage = fmt::format("unable to parse storyboard {}: {}", storyboardId, err.getMessage());
            warn(errorMessage);
            recordSpanError(dbSpan, errorMessage, "InvalidData", err.getCode());
            return Result<Storyboard>{err};
        }
        if (fetchSpan)
            fetchSpan->setSuccess();

        if (dbSpan)
            dbSpan->setSuccess();
        return Result<Storyboard>{result.getValue().value()};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage =
            fmt::format("MongoDB exception caught while finding storyboard {}: {}", storyboardId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
        }
        if (dbSpan) {
            dbSpan->recordException(e);
        }
        recordSpanError(dbSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<Storyboard>{ServerError(ServerError::DatabaseError, errorMessage)};
    } catch (const std::exception &e) {
        std::string errorMessage =
            fmt::format("Standard exception caught while finding storyboard {}: {}", storyboardId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
        }
        if (dbSpan) {
            dbSpan->recordException(e);
        }
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<Storyboard>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown exception caught while finding storyboard {}", storyboardId);
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->setError(errorMessage);
        }
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<Storyboard>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<std::vector<creatures::Storyboard>> Database::listStoryboards(const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.listStoryboards, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.listStoryboards", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", STORYBOARDS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
    }

    info("attempting to list all Storyboards");

    auto storyboardList = std::vector<Storyboard>{};

    try {
        document query_doc{};
        document sort_doc{};
        // Newest-first so the Console's storyboard list defaults to most-recently-edited.
        sort_doc << "updated_at" << -1;

        auto collectionResult = getCollection(STORYBOARDS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage = fmt::format("unable to get the storyboards collection: {}", err.getMessage());
            critical(errorMessage);
            recordSpanError(dbSpan, errorMessage, "DatabaseError", err.getCode());
            return Result<std::vector<Storyboard>>{err};
        }
        auto collection = collectionResult.getValue().value();

        auto mongoSpan = creatures::observability->createChildOperationSpan("listStoryboards.mongoQuery", dbSpan);
        mongocxx::options::find opts;
        opts.sort(sort_doc.view());
        mongocxx::cursor cursor = collection.find(query_doc.view(), opts);

        for (auto doc : cursor) {
            auto storyboardSpan =
                creatures::observability->createChildOperationSpan("listStoryboards.create-storyboard", mongoSpan);

            auto jsonResult = JsonParser::bsonToJson(doc, "storyboard document", storyboardSpan);
            if (!jsonResult.isSuccess()) {
                auto err = jsonResult.getError().value();
                if (storyboardSpan) {
                    storyboardSpan->setError(err.getMessage());
                    storyboardSpan->setAttribute("error.type", "JsonParsingException");
                    storyboardSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
                }
                continue;
            }
            json j = jsonResult.getValue().value();

            auto result = storyboardFromJson(j, storyboardSpan);
            if (!result.isSuccess()) {
                auto err = result.getError().value();
                std::string errorMessage =
                    fmt::format("Data format error while listing storyboards: {}", err.getMessage());
                critical(errorMessage);
                if (storyboardSpan) {
                    storyboardSpan->setError(errorMessage);
                    storyboardSpan->setAttribute("error.type", "DataFormatException");
                    storyboardSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
                }
                recordSpanError(dbSpan, errorMessage, "DataFormatException", err.getCode());
                return Result<std::vector<Storyboard>>{err};
            }
            storyboardList.push_back(result.getValue().value());

            if (storyboardSpan) {
                storyboardSpan->setAttribute("storyboard.id", result.getValue().value().id);
                storyboardSpan->setSuccess();
            }
        }
        if (mongoSpan) {
            mongoSpan->setAttribute("storyboards.count", static_cast<int64_t>(storyboardList.size()));
            mongoSpan->setSuccess();
        }

        debug("found {} storyboards", storyboardList.size());
        if (dbSpan) {
            dbSpan->setAttribute("storyboards.count", static_cast<int64_t>(storyboardList.size()));
            dbSpan->setSuccess();
        }
        return Result<std::vector<Storyboard>>{storyboardList};

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Failed to list storyboards: {}", e.what());
        error(errorMessage);
        if (dbSpan) {
            dbSpan->recordException(e);
        }
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<std::vector<Storyboard>>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = "Failed to list storyboards: unknown error";
        error(errorMessage);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<std::vector<Storyboard>>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures
