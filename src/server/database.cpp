
#include "config.h"

#include <sstream>
#include <string>

#include "spdlog/spdlog.h"

#include "server/database.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/pool.hpp>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "util/Result.h"

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::stream::document;

namespace creatures {

Database::Database(const std::string &mongoURI_) : mongoURI(mongoURI_), mongoPool(mongocxx::uri{mongoURI_}) {
    info("starting up database connection for {}. Database name {} will be used", mongoURI_, DB_NAME);
}

Result<mongocxx::collection> Database::getCollection(const std::string &collectionName) {

    debug("getting a handle to collection {}", collectionName);

    // Don't do this if we can't ping the server (ie, short-circuit quickly)
    if (!serverPingable.load()) {
        const std::string errorMessage = "Unable to get a collection because the server is not pingable";
        critical(errorMessage);
        return Result<mongocxx::collection>{ServerError(ServerError::DatabaseError, errorMessage)};
    }

    // Acquire a MongoDB client from the pool
    try {
        thread_local auto client = mongoPool.acquire();
        return (*client)[DB_NAME][collectionName];
    } catch (const std::exception &e) {
        const std::string errorMessage =
            fmt::format("Internal error while getting the collection '{}': {}", collectionName, e.what());
        critical(errorMessage);
        return Result<mongocxx::collection>{ServerError(ServerError::DatabaseError, errorMessage)};
    } catch (...) {
        const std::string errorMessage = fmt::format("Unknown error while getting the collection '{}'", collectionName);
        critical(errorMessage);
        return Result<mongocxx::collection>{ServerError(ServerError::DatabaseError, errorMessage)};
    }
}

// Return a copy of the atomic
bool Database::isServerPingable() const { return serverPingable.load(); }

void Database::performHealthCheck() {

    try {
        const auto ping_cmd = make_document(kvp("ping", 1));

        const auto client = mongoPool.acquire();
        mongocxx::database db = (*client)[DB_NAME];
        db.run_command(ping_cmd.view());

        // If we get here, the server is pingable
        if (!serverPingable.load()) {
            info("Database is now online!");
        }
        serverPingable.store(true);
    } catch (const std::exception &e) {

        if (serverPingable.load()) {

            // The server just went offline
            const std::string errorMessage =
                fmt::format("Database now offline. Error pinging the server: {}", e.what());
            error(errorMessage);
        } else {

            // We're still down. Throw a trace message just in case.
            const std::string errorMessage = fmt::format("Error pinging the server: {}", e.what());
            trace(errorMessage);
        }

        serverPingable.store(false);
    } catch (...) {
        const std::string errorMessage = "Unknown error pinging the server";
        critical(errorMessage);
        serverPingable.store(false);
    }
}

Result<bool> Database::checkJsonField(const nlohmann::json &jsonObj, const std::string &fieldName) {
    if (!jsonObj.contains(fieldName)) {
        const std::string errorMessage = fmt::format("JSON is missing required field: {}", fieldName);
        warn(errorMessage);
        return Result<bool>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    return true;
}

Result<void> Database::ensureAdHocAnimationIndexes(uint32_t ttlHours) {
    if (ttlHours == 0) {
        const std::string errorMessage = "Ad-hoc animation TTL hours must be greater than zero";
        warn(errorMessage);
        return Result<void>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto ttlSeconds = std::chrono::seconds(ttlHours * 3600ULL);

    try {
        auto collectionResult = getCollection(ADHOC_ANIMATIONS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            return Result<void>{collectionResult.getError().value()};
        }
        auto collection = collectionResult.getValue().value();

        bsoncxx::builder::stream::document ttlIndex;
        ttlIndex << "created_at" << 1;

        mongocxx::options::index options;
        options.expire_after(ttlSeconds);
        options.name("created_at_ttl");

        collection.create_index(ttlIndex.view(), options);
        info("Ensured TTL index on '{}' (created_at + {} hours)", ADHOC_ANIMATIONS_COLLECTION, ttlHours);

        return Result<void>{};

    } catch (const std::exception &e) {
        std::string errorMessage =
            fmt::format("Failed to ensure TTL index on {}: {}", ADHOC_ANIMATIONS_COLLECTION, e.what());
        error(errorMessage);
        return Result<void>{ServerError(ServerError::DatabaseError, errorMessage)};
    }
}

} // namespace creatures
