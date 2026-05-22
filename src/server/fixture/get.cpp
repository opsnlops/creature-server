
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

Result<json> Database::getFixtureJson(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &parentSpan) {
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getFixtureJson", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", FIXTURES_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("fixture.id", fixtureId);
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
    }

    if (fixtureId.empty()) {
        std::string errorMessage = "unable to get a fixture because the id was empty";
        info(errorMessage);
        if (dbSpan) {
            dbSpan->setError(errorMessage);
            dbSpan->setAttribute("error.type", "InvalidData");
            dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
        return Result<json>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto collectionResult = getCollection(FIXTURES_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        std::string errorMessage = fmt::format("unable to get the fixture collection: {}", err.getMessage());
        critical(errorMessage);
        if (dbSpan) {
            dbSpan->setError(errorMessage);
            dbSpan->setAttribute("error.type", "DatabaseError");
            dbSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        return Result<json>{err};
    }
    auto collection = collectionResult.getValue().value();

    std::shared_ptr<OperationSpan> mongoSpan;
    try {
        mongoSpan = creatures::observability->createChildOperationSpan("getFixtureJson.mongoQuery", dbSpan);

        auto query = document{} << "id" << fixtureId << finalize;
        auto maybe_result = collection.find_one(query.view());
        mongoSpan->setSuccess();

        if (!maybe_result) {
            std::string errorMessage = fmt::format("Fixture not found: {}", fixtureId);
            warn(errorMessage);
            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "NotFound");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::NotFound));
            }
            return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
        }

        auto convertSpan = creatures::observability->createChildOperationSpan("getFixtureJson.json::parse", dbSpan);
        auto jsonResult =
            JsonParser::bsonToJson(maybe_result->view(), fmt::format("fixture {}", fixtureId), convertSpan);
        if (!jsonResult.isSuccess()) {
            warn("Failed to convert BSON to JSON for fixture ID: {} - {}", fixtureId,
                 jsonResult.getError().value().getMessage());
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
            mongoSpan->setAttribute("error.type", "MongoDBException");
            mongoSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::DatabaseError));
        }
        return Result<json>{ServerError(ServerError::DatabaseError, errorMessage)};
    } catch (const std::exception &e) {
        std::string errorMessage =
            fmt::format("Standard exception caught while finding fixture {}: {}", fixtureId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setAttribute("error.type", "StandardException");
            mongoSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
        }
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown exception caught while finding fixture {}", fixtureId);
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->setError(errorMessage);
        }
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<creatures::DmxFixture> Database::getFixture(const fixtureId_t &fixtureId,
                                                   const std::shared_ptr<OperationSpan> &parentSpan) {

    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getFixture", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("fixture.id", fixtureId);
    }

    if (fixtureId.empty()) {
        std::string errorMessage = "unable to get a fixture because the id was empty";
        warn(errorMessage);
        if (dbSpan) {
            dbSpan->setError(errorMessage);
            dbSpan->setAttribute("error.type", "InvalidData");
            dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
        return Result<DmxFixture>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto jsonSpan = creatures::observability->createChildOperationSpan("getFixture.getFixtureJson", dbSpan);
    auto fixtureJson = getFixtureJson(fixtureId, jsonSpan);
    if (!fixtureJson.isSuccess()) {
        auto err = fixtureJson.getError().value();
        std::string errorMessage =
            fmt::format("unable to get a fixture by ID: {}", fixtureJson.getError()->getMessage());
        warn(errorMessage);
        if (jsonSpan) {
            jsonSpan->setError(errorMessage);
            jsonSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        return Result<DmxFixture>{err};
    }
    jsonSpan->setSuccess();

    auto fetchSpan = creatures::observability->createChildOperationSpan("getFixture.fixtureFromJson", dbSpan);
    auto result = fixtureFromJson(fixtureJson.getValue().value(), fetchSpan);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        std::string errorMessage = fmt::format("unable to get a fixture by ID: {}", result.getError()->getMessage());
        warn(errorMessage);
        if (fetchSpan) {
            fetchSpan->setError(errorMessage);
            fetchSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        return Result<DmxFixture>{err};
    }
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
