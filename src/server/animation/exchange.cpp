// Ad-hoc exchange persistence (issue #150): one document per streaming ad-hoc
// session in a TTL'd collection, so a whole conversation turn can be listed and
// exported after the fact. Spans conform to docs/database-observability.md.

#include <chrono>
#include <vector>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/index.hpp>

#include "model/AdHocExchange.h"
#include "server/config.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<ObservabilityManager> observability;

using bsoncxx::builder::stream::document;

namespace {

// Shared decode of one exchange document (JSON body + BSON created_at).
Result<AdHocExchangeRecord> recordFromBson(const bsoncxx::document::view &doc, const std::string &context,
                                           const std::shared_ptr<OperationSpan> &span) {
    auto jsonResult = JsonParser::bsonToJson(doc, context, span);
    if (!jsonResult.isSuccess()) {
        return Result<AdHocExchangeRecord>{jsonResult.getError().value()};
    }
    auto exchangeResult = adHocExchangeFromJson(jsonResult.getValue().value());
    if (!exchangeResult.isSuccess()) {
        return Result<AdHocExchangeRecord>{exchangeResult.getError().value()};
    }
    AdHocExchangeRecord record;
    record.exchange = exchangeResult.getValue().value();
    if (doc["created_at"] && doc["created_at"].type() == bsoncxx::type::k_date) {
        auto millis = doc["created_at"].get_date().value;
        record.createdAt = std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(millis));
    } else {
        record.createdAt = std::chrono::system_clock::now();
    }
    return record;
}

} // namespace

Result<void> Database::ensureAdHocExchangeIndexes(uint32_t ttlHours) {
    if (ttlHours == 0) {
        const std::string errorMessage = "Ad-hoc exchange TTL hours must be greater than zero";
        warn(errorMessage);
        return Result<void>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto ttlSeconds = std::chrono::seconds(ttlHours * 3600ULL);

    try {
        auto collectionResult = getCollection(ADHOC_EXCHANGES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            return Result<void>{collectionResult.getError().value()};
        }
        auto collectionLease = collectionResult.getValue().value();
        auto &collection = collectionLease->collection();

        document ttlIndex;
        ttlIndex << "created_at" << 1;
        mongocxx::options::index ttlOptions;
        ttlOptions.expire_after(ttlSeconds);
        ttlOptions.name("created_at_ttl");
        collection.create_index(ttlIndex.view(), ttlOptions);

        // Lookups and finalize updates are all by session_id.
        document sessionIndex;
        sessionIndex << "session_id" << 1;
        mongocxx::options::index sessionOptions;
        sessionOptions.name("session_id_unique");
        sessionOptions.unique(true);
        collection.create_index(sessionIndex.view(), sessionOptions);

        info("Ensured indexes on '{}' (created_at TTL + {} hours, unique session_id)", ADHOC_EXCHANGES_COLLECTION,
             ttlHours);
        return Result<void>{};

    } catch (const std::exception &e) {
        std::string errorMessage =
            fmt::format("Failed to ensure indexes on {}: {}", ADHOC_EXCHANGES_COLLECTION, e.what());
        error(errorMessage);
        return Result<void>{ServerError(ServerError::DatabaseError, errorMessage)};
    }
}

Result<void> Database::insertAdHocExchange(const creatures::AdHocExchange &exchange,
                                           std::chrono::system_clock::time_point createdAt,
                                           std::shared_ptr<OperationSpan> parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.insertAdHocExchange, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.insertAdHocExchange", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ADHOC_EXCHANGES_COLLECTION);
        dbSpan->setAttribute("database.operation", "insert_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("session.id", exchange.session_id);
    }

    try {
        auto jsonString = adHocExchangeToJson(exchange).dump();
        auto bsonResult =
            JsonParser::jsonStringToBson(jsonString, fmt::format("adhoc exchange {}", exchange.session_id), dbSpan);
        if (!bsonResult.isSuccess()) {
            auto err = bsonResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<void>{err};
        }

        auto collectionResult = getCollection(ADHOC_EXCHANGES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "DatabaseError", err.getCode());
            return Result<void>{err};
        }
        auto collectionLease = collectionResult.getValue().value();
        auto &collection = collectionLease->collection();

        auto mongoSpan = creatures::observability->createChildOperationSpan("insertAdHocExchange.mongoQuery", dbSpan);
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(createdAt.time_since_epoch());
        auto finalDoc = document{} << "created_at" << bsoncxx::types::b_date{millis}
                                   << bsoncxx::builder::stream::concatenate(bsonResult.getValue().value().view())
                                   << bsoncxx::builder::stream::finalize;
        collection.insert_one(finalDoc.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        info("Inserted ad-hoc exchange {} into {}", exchange.session_id, ADHOC_EXCHANGES_COLLECTION);
        if (dbSpan)
            dbSpan->setSuccess();
        return Result<void>{};

    } catch (const std::exception &e) {
        std::string errorMessage =
            fmt::format("Failed to insert ad-hoc exchange {}: {}", exchange.session_id, e.what());
        error(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::DatabaseError);
        return Result<void>{ServerError(ServerError::DatabaseError, errorMessage)};
    }
}

Result<void> Database::finalizeAdHocExchange(const creatures::AdHocExchange &exchange,
                                             std::shared_ptr<OperationSpan> parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.finalizeAdHocExchange, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.finalizeAdHocExchange", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ADHOC_EXCHANGES_COLLECTION);
        dbSpan->setAttribute("database.operation", "update_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("session.id", exchange.session_id);
        dbSpan->setAttribute("exchange.status", exchange.status);
    }

    try {
        auto jsonString = adHocExchangeToJson(exchange).dump();
        auto bsonResult =
            JsonParser::jsonStringToBson(jsonString, fmt::format("adhoc exchange {}", exchange.session_id), dbSpan);
        if (!bsonResult.isSuccess()) {
            auto err = bsonResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<void>{err};
        }

        auto collectionResult = getCollection(ADHOC_EXCHANGES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "DatabaseError", err.getCode());
            return Result<void>{err};
        }
        auto collectionLease = collectionResult.getValue().value();
        auto &collection = collectionLease->collection();

        auto mongoSpan = creatures::observability->createChildOperationSpan("finalizeAdHocExchange.mongoQuery", dbSpan);
        auto filter = document{} << "session_id" << exchange.session_id << bsoncxx::builder::stream::finalize;
        // $set the whole JSON body; created_at (the TTL clock) isn't in the
        // JSON codec, so the sweep deadline is unaffected by finalization.
        auto update = document{} << "$set" << bsoncxx::builder::stream::open_document
                                 << bsoncxx::builder::stream::concatenate(bsonResult.getValue().value().view())
                                 << bsoncxx::builder::stream::close_document << bsoncxx::builder::stream::finalize;
        auto result = collection.update_one(filter.view(), update.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!result || result->matched_count() == 0) {
            std::string errorMessage =
                fmt::format("No ad-hoc exchange {} to finalize (TTL'd already?)", exchange.session_id);
            warn(errorMessage);
            recordSpanError(dbSpan, errorMessage, "NotFound", ServerError::NotFound);
            return Result<void>{ServerError(ServerError::NotFound, errorMessage)};
        }

        info("Finalized ad-hoc exchange {} as '{}' ({} parts)", exchange.session_id, exchange.status,
             exchange.parts.size());
        if (dbSpan)
            dbSpan->setSuccess();
        return Result<void>{};

    } catch (const std::exception &e) {
        std::string errorMessage =
            fmt::format("Failed to finalize ad-hoc exchange {}: {}", exchange.session_id, e.what());
        error(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::DatabaseError);
        return Result<void>{ServerError(ServerError::DatabaseError, errorMessage)};
    }
}

Result<std::vector<AdHocExchangeRecord>> Database::listAdHocExchanges(int limit,
                                                                      std::shared_ptr<OperationSpan> parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.listAdHocExchanges, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.listAdHocExchanges", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ADHOC_EXCHANGES_COLLECTION);
        dbSpan->setAttribute("database.operation", "find");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("query.limit", static_cast<int64_t>(limit));
    }

    try {
        auto collectionResult = getCollection(ADHOC_EXCHANGES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "DatabaseError", err.getCode());
            return Result<std::vector<AdHocExchangeRecord>>{err};
        }
        auto collectionLease = collectionResult.getValue().value();
        auto &collection = collectionLease->collection();

        auto mongoSpan = creatures::observability->createChildOperationSpan("listAdHocExchanges.mongoQuery", dbSpan);
        mongocxx::options::find options;
        mongo::applyOperationDeadline(options);
        document sortDoc;
        sortDoc << "created_at" << -1;
        options.sort(sortDoc.view());
        if (limit > 0) {
            options.limit(limit);
        }

        std::vector<AdHocExchangeRecord> records;
        auto cursor = collection.find({}, options);
        for (const auto &doc : cursor) {
            auto recordResult = recordFromBson(doc, "adhoc exchange list", mongoSpan);
            if (!recordResult.isSuccess()) {
                auto err = recordResult.getError().value();
                recordSpanError(dbSpan, err.getMessage(), "DataFormatException", err.getCode());
                return Result<std::vector<AdHocExchangeRecord>>{err};
            }
            records.push_back(recordResult.getValue().value());
        }
        if (mongoSpan) {
            mongoSpan->setAttribute("exchanges.count", static_cast<int64_t>(records.size()));
            mongoSpan->setSuccess();
        }
        if (dbSpan) {
            dbSpan->setAttribute("exchanges.count", static_cast<int64_t>(records.size()));
            dbSpan->setSuccess();
        }
        return Result<std::vector<AdHocExchangeRecord>>{records};

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Failed to list ad-hoc exchanges: {}", e.what());
        error(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::DatabaseError);
        return Result<std::vector<AdHocExchangeRecord>>{ServerError(ServerError::DatabaseError, errorMessage)};
    }
}

Result<AdHocExchangeRecord> Database::getAdHocExchange(const std::string &sessionId,
                                                       std::shared_ptr<OperationSpan> parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getAdHocExchange, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getAdHocExchange", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ADHOC_EXCHANGES_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("session.id", sessionId);
    }

    try {
        auto collectionResult = getCollection(ADHOC_EXCHANGES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "DatabaseError", err.getCode());
            return Result<AdHocExchangeRecord>{err};
        }
        auto collectionLease = collectionResult.getValue().value();
        auto &collection = collectionLease->collection();

        auto mongoSpan = creatures::observability->createChildOperationSpan("getAdHocExchange.mongoQuery", dbSpan);
        auto filter = document{} << "session_id" << sessionId << bsoncxx::builder::stream::finalize;
        mongocxx::options::find readOptions;
        mongo::applyOperationDeadline(readOptions);
        auto maybeDoc = collection.find_one(filter.view(), readOptions);
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!maybeDoc) {
            std::string errorMessage = fmt::format("No ad-hoc exchange found for session {}", sessionId);
            recordSpanError(dbSpan, errorMessage, "NotFound", ServerError::NotFound);
            return Result<AdHocExchangeRecord>{ServerError(ServerError::NotFound, errorMessage)};
        }

        auto recordResult = recordFromBson(maybeDoc->view(), fmt::format("adhoc exchange {}", sessionId), dbSpan);
        if (!recordResult.isSuccess()) {
            auto err = recordResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "DataFormatException", err.getCode());
            return recordResult;
        }
        if (dbSpan)
            dbSpan->setSuccess();
        return recordResult;

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Failed to get ad-hoc exchange {}: {}", sessionId, e.what());
        error(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::DatabaseError);
        return Result<AdHocExchangeRecord>{ServerError(ServerError::DatabaseError, errorMessage)};
    }
}

} // namespace creatures
