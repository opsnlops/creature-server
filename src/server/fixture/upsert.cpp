
#include "server/config.h"

#include <spdlog/spdlog.h>
#include <string>

#include <nlohmann/json.hpp>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>

#include "model/DmxFixture.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/Result.cpp"
#include "util/cache.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::stream::document;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObjectCache<fixtureId_t, DmxFixture>> fixtureCache;
extern std::shared_ptr<ObservabilityManager> observability;

// Conforms to docs/database-observability.md (issue #17).

Result<creatures::DmxFixture> Database::upsertFixture(const std::string &fixtureJson,
                                                      const std::shared_ptr<OperationSpan> &parentSpan) {

    if (!parentSpan) {
        warn("no parent span provided for Database.upsertFixture, creating a root span");
    }
    auto upsertSpan = creatures::observability->createChildOperationSpan("Database.upsertFixture", parentSpan);
    if (upsertSpan) {
        upsertSpan->setAttribute("database.collection", FIXTURES_COLLECTION);
        upsertSpan->setAttribute("database.operation", "update_one");
        upsertSpan->setAttribute("database.system", "mongodb");
        upsertSpan->setAttribute("database.name", DB_NAME);
    }

    info("attempting to upsert a DmxFixture in the database");
    try {

        auto parseJsonSpan = creatures::observability->createChildOperationSpan("upsertFixture.parse-json", upsertSpan);
        auto jsonResult = JsonParser::parseJsonString(fixtureJson, "fixture upsert", parseJsonSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            recordSpanError(upsertSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<DmxFixture>{err};
        }
        auto jsonObject = jsonResult.getValue().value();

        auto fixtureResult = fixtureFromJson(jsonObject, upsertSpan);
        if (!fixtureResult.isSuccess()) {
            auto err = fixtureResult.getError().value();
            auto errorMessage = fmt::format("Error while creating a fixture from JSON: {}", err.getMessage());
            recordSpanError(upsertSpan, errorMessage, "InvalidData", err.getCode());
            warn(errorMessage);
            return Result<DmxFixture>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        auto fixture = fixtureResult.getValue().value();
        if (upsertSpan) {
            // Set fixture.id early so subsequent error paths carry it.
            upsertSpan->setAttribute("fixture.id", fixture.id);
        }

        // Convert the JSON string into BSON
        auto bsonSpan = creatures::observability->createChildOperationSpan("upsertFixture.json-to-bson", upsertSpan);
        auto bsonResult = JsonParser::jsonStringToBson(fixtureJson, fmt::format("fixture {}", fixture.id), bsonSpan);
        if (!bsonResult.isSuccess()) {
            auto err = bsonResult.getError().value();
            recordSpanError(upsertSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<DmxFixture>{err};
        }
        auto bsonDoc = bsonResult.getValue().value();

        auto collectionSpan =
            creatures::observability->createChildOperationSpan("upsertFixture.get-collection", upsertSpan);
        auto collectionResult = getCollection(FIXTURES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage = fmt::format("database error while getting fixture: {}", err.getMessage());
            if (collectionSpan) {
                collectionSpan->setError(errorMessage);
                collectionSpan->setAttribute("error.type", "DatabaseError");
                collectionSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::DatabaseError));
            }
            recordSpanError(upsertSpan, errorMessage, "DatabaseError", err.getCode());
            warn(errorMessage);
            return Result<DmxFixture>{err};
        }
        auto collection = collectionResult.getValue().value();
        if (collectionSpan)
            collectionSpan->setSuccess();

        auto mongoSpan = creatures::observability->createChildOperationSpan("upsertFixture.mongoQuery", upsertSpan);
        auto id = fixture.id;
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

        fixtureCache->put(fixture.id, fixture);

        if (upsertSpan) {
            upsertSpan->setAttribute("fixture.name", fixture.name);
            upsertSpan->setAttribute("fixture.type", fixtureTypeToString(fixture.type));
            upsertSpan->setAttribute("fixture.channels_count", static_cast<int64_t>(fixture.channels.size()));
            upsertSpan->setAttribute("fixture.patterns_count", static_cast<int64_t>(fixture.patterns.size()));
            upsertSpan->setAttribute("fixture.bindings_count", static_cast<int64_t>(fixture.bindings.size()));
            upsertSpan->setAttribute("fixture.universe.set", fixture.assigned_universe.has_value());
            if (fixture.assigned_universe.has_value()) {
                upsertSpan->setAttribute("fixture.universe", static_cast<int64_t>(*fixture.assigned_universe));
            }
            upsertSpan->setSuccess();
        }

        info("Fixture upserted in the database: {}", fixture.id);
        return Result<DmxFixture>{fixture};

    } catch (const mongocxx::exception &e) {
        auto errorMessage =
            fmt::format("Error (mongocxx::exception) while upserting a fixture in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan)
            upsertSpan->recordException(e);
        recordSpanError(upsertSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<DmxFixture>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const bsoncxx::exception &e) {
        auto errorMessage =
            fmt::format("Error (bsoncxx::exception) while upserting a fixture in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan)
            upsertSpan->recordException(e);
        recordSpanError(upsertSpan, errorMessage, "JsonParsingException", ServerError::InvalidData);
        return Result<DmxFixture>{ServerError(ServerError::InvalidData, errorMessage)};
    } catch (...) {
        std::string errorMessage = "Unknown error while adding a fixture to the database";
        critical(errorMessage);
        recordSpanError(upsertSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<DmxFixture>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<void> Database::setFixtureUniverse(const fixtureId_t &fixtureId, std::optional<universe_t> universe,
                                          const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.setFixtureUniverse, creating a root span");
    }
    auto span = creatures::observability->createChildOperationSpan("Database.setFixtureUniverse", parentSpan);
    if (span) {
        span->setAttribute("database.collection", FIXTURES_COLLECTION);
        span->setAttribute("database.operation", "update_one");
        span->setAttribute("database.system", "mongodb");
        span->setAttribute("database.name", DB_NAME);
        span->setAttribute("fixture.id", fixtureId);
        span->setAttribute("fixture.universe.set", universe.has_value());
        if (universe.has_value()) {
            span->setAttribute("fixture.universe", static_cast<int64_t>(*universe));
        }
    }

    if (fixtureId.empty()) {
        std::string errorMessage = "setFixtureUniverse called with empty fixtureId";
        warn(errorMessage);
        recordSpanError(span, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<void>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    try {
        auto collectionResult = getCollection(FIXTURES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(span, err.getMessage(), "DatabaseError", err.getCode());
            return Result<void>{err};
        }
        auto collection = collectionResult.getValue().value();

        auto mongoSpan = creatures::observability->createChildOperationSpan("setFixtureUniverse.mongoQuery", span);
        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << fixtureId;

        bsoncxx::builder::stream::document update_builder;
        if (universe.has_value()) {
            update_builder << "$set" << bsoncxx::builder::stream::open_document << "assigned_universe"
                           << static_cast<int64_t>(*universe) << bsoncxx::builder::stream::close_document;
        } else {
            update_builder << "$unset" << bsoncxx::builder::stream::open_document << "assigned_universe" << ""
                           << bsoncxx::builder::stream::close_document;
        }

        auto result = collection.update_one(filter_builder.view(), update_builder.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!result || result->matched_count() == 0) {
            std::string errorMessage = fmt::format("Fixture {} not found while setting universe", fixtureId);
            warn(errorMessage);
            recordSpanError(span, errorMessage, "NotFound", ServerError::NotFound);
            return Result<void>{ServerError(ServerError::NotFound, errorMessage)};
        }

        if (span)
            span->setSuccess();
        return Result<void>{};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage =
            fmt::format("Error (mongocxx::exception) while setting fixture universe: {}", e.what());
        error(errorMessage);
        if (span)
            span->recordException(e);
        recordSpanError(span, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = "Unknown error while setting fixture universe";
        critical(errorMessage);
        recordSpanError(span, errorMessage, "std::exception", ServerError::InternalError);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<void> Database::deleteFixture(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.deleteFixture, creating a root span");
    }
    auto span = creatures::observability->createChildOperationSpan("Database.deleteFixture", parentSpan);
    if (span) {
        span->setAttribute("database.collection", FIXTURES_COLLECTION);
        span->setAttribute("database.operation", "delete_one");
        span->setAttribute("database.system", "mongodb");
        span->setAttribute("database.name", DB_NAME);
        span->setAttribute("fixture.id", fixtureId);
    }

    if (fixtureId.empty()) {
        std::string errorMessage = "deleteFixture called with empty fixtureId";
        warn(errorMessage);
        recordSpanError(span, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<void>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    try {
        auto collectionResult = getCollection(FIXTURES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(span, err.getMessage(), "DatabaseError", err.getCode());
            return Result<void>{err};
        }
        auto collection = collectionResult.getValue().value();

        auto mongoSpan = creatures::observability->createChildOperationSpan("deleteFixture.mongoQuery", span);
        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << fixtureId;

        auto result = collection.delete_one(filter_builder.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!result || result->deleted_count() == 0) {
            std::string errorMessage = fmt::format("Fixture {} not found while deleting", fixtureId);
            warn(errorMessage);
            recordSpanError(span, errorMessage, "NotFound", ServerError::NotFound);
            return Result<void>{ServerError(ServerError::NotFound, errorMessage)};
        }

        fixtureCache->remove(fixtureId);

        if (span)
            span->setSuccess();
        return Result<void>{};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage = fmt::format("Error while deleting fixture {}: {}", fixtureId, e.what());
        error(errorMessage);
        if (span)
            span->recordException(e);
        recordSpanError(span, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown error while deleting fixture {}", fixtureId);
        critical(errorMessage);
        recordSpanError(span, errorMessage, "std::exception", ServerError::InternalError);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures
