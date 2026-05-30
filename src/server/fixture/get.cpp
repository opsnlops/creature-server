
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
#include "model/DmxFixture.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"
#include "util/cache.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::stream::document;
using bsoncxx::builder::stream::finalize;
using json = nlohmann::json;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObjectCache<fixtureId_t, DmxFixture>> fixtureCache;
extern std::shared_ptr<ObservabilityManager> observability;

// Conforms to docs/database-observability.md (issue #17).

Result<json> Database::getFixtureJson(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getFixtureJson, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getFixtureJson", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", FIXTURES_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("fixture.id", fixtureId);
    }

    auto setSpanError = [&](const std::string &msg, const std::string &type, ServerError::Code code) {
        if (dbSpan) {
            dbSpan->setError(msg);
            dbSpan->setAttribute("error.type", type);
            dbSpan->setAttribute("error.code", static_cast<int64_t>(code));
        }
    };

    if (fixtureId.empty()) {
        std::string errorMessage = "unable to get a fixture because the id was empty";
        info(errorMessage);
        setSpanError(errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<json>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto collectionResult = getCollection(FIXTURES_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        std::string errorMessage = fmt::format("unable to get the fixture collection: {}", err.getMessage());
        critical(errorMessage);
        setSpanError(errorMessage, "DatabaseError", err.getCode());
        return Result<json>{err};
    }
    auto collection = collectionResult.getValue().value();

    std::shared_ptr<OperationSpan> mongoSpan;
    try {
        mongoSpan = creatures::observability->createChildOperationSpan("getFixtureJson.mongoQuery", dbSpan);

        auto query = document{} << "id" << fixtureId << finalize;
        auto maybe_result = collection.find_one(query.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!maybe_result) {
            std::string errorMessage = fmt::format("Fixture not found: {}", fixtureId);
            warn(errorMessage);
            setSpanError(errorMessage, "NotFound", ServerError::NotFound);
            return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
        }

        auto convertSpan = creatures::observability->createChildOperationSpan("getFixtureJson.bson-to-json", dbSpan);
        auto jsonResult =
            JsonParser::bsonToJson(maybe_result->view(), fmt::format("fixture {}", fixtureId), convertSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            warn("Failed to convert BSON to JSON for fixture ID: {} - {}", fixtureId, err.getMessage());
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
            fmt::format("MongoDB exception caught while finding fixture {}: {}", fixtureId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
            mongoSpan->setAttribute("error.type", "MongoDBException");
            mongoSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::DatabaseError));
        }
        if (dbSpan)
            dbSpan->recordException(e);
        setSpanError(errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<json>{ServerError(ServerError::DatabaseError, errorMessage)};
    } catch (const std::exception &e) {
        std::string errorMessage =
            fmt::format("Standard exception caught while finding fixture {}: {}", fixtureId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
            mongoSpan->setAttribute("error.type", "std::exception");
            mongoSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
        }
        if (dbSpan)
            dbSpan->recordException(e);
        setSpanError(errorMessage, "std::exception", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown exception caught while finding fixture {}", fixtureId);
        critical(errorMessage);
        if (mongoSpan)
            mongoSpan->setError(errorMessage);
        setSpanError(errorMessage, "std::exception", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<creatures::DmxFixture> Database::getFixture(const fixtureId_t &fixtureId,
                                                   const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getFixture, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getFixture", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", FIXTURES_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("fixture.id", fixtureId);
    }

    auto setSpanError = [&](const std::string &msg, const std::string &type, ServerError::Code code) {
        if (dbSpan) {
            dbSpan->setError(msg);
            dbSpan->setAttribute("error.type", type);
            dbSpan->setAttribute("error.code", static_cast<int64_t>(code));
        }
    };

    if (fixtureId.empty()) {
        std::string errorMessage = "unable to get a fixture because the id was empty";
        warn(errorMessage);
        setSpanError(errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<DmxFixture>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto jsonSpan = creatures::observability->createChildOperationSpan("getFixture.getFixtureJson", dbSpan);
    auto fixtureJson = getFixtureJson(fixtureId, jsonSpan);
    if (!fixtureJson.isSuccess()) {
        auto err = fixtureJson.getError().value();
        std::string errorMessage = fmt::format("unable to get a fixture by ID: {}", err.getMessage());
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
            jsonSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        setSpanError(errorMessage, etype, err.getCode());
        return Result<DmxFixture>{err};
    }
    if (jsonSpan)
        jsonSpan->setSuccess();

    auto fetchSpan = creatures::observability->createChildOperationSpan("getFixture.fixtureFromJson", dbSpan);
    auto result = fixtureFromJson(fixtureJson.getValue().value(), fetchSpan);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        std::string errorMessage = fmt::format("unable to get a fixture by ID: {}", err.getMessage());
        warn(errorMessage);
        if (fetchSpan) {
            fetchSpan->setError(errorMessage);
            fetchSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        setSpanError(errorMessage, "InvalidData", err.getCode());
        return Result<DmxFixture>{err};
    }
    if (fetchSpan)
        fetchSpan->setSuccess();

    auto fixture = result.getValue().value();
    fixtureCache->put(fixtureId, fixture);
    if (dbSpan) {
        dbSpan->setAttribute("cache.status", "updated");
        dbSpan->setSuccess();
    }
    return Result<DmxFixture>{fixture};
}

} // namespace creatures
