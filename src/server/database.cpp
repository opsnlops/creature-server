
#include "config.h"

#include <string>
#include <sstream>

#include "spdlog/spdlog.h"


#include "server/database.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/pool.hpp>

#include "exception/exception.h"
#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    Database::Database(mongocxx::pool &pool) : pool(pool) {
        info("starting up database connection");
    }

    mongocxx::collection Database::getCollection(const std::string &collectionName) {

        debug("connecting to a collection");

        // Don't do this if we can't ping the server (ie, short-circuit quickly)
        if(!serverPingable.load()) {
            std::string errorMessage = "Unable to get a collection because the server is not pingable";
            critical(errorMessage);
            throw creatures::DatabaseError(errorMessage);
        }

        // Acquire a MongoDB client from the pool
        try {
            auto client = pool.acquire();
            auto collection = (*client)[DB_NAME][collectionName];
            return collection;
        }
        catch (const std::exception &e) {
            std::string errorMessage = fmt::format("Internal error while getting the collection '{}': {}", collectionName, e.what());
            critical(errorMessage);
            throw creatures::DatabaseError(errorMessage);
        }
        catch ( ... ) {
            std::string errorMessage = fmt::format("Unknown error while getting the collection '{}'", collectionName);
            critical(errorMessage);
            throw creatures::DatabaseError(errorMessage);

        }

    }

    // Return a copy of the atomic
    bool Database::isServerPingable() {
        return serverPingable.load();
    }

    void Database::performHealthCheck() {

        try {
            const auto ping_cmd = make_document(kvp("ping", 1));

            auto client = pool.acquire();
            mongocxx::database db = (*client)[DB_NAME];
            db.run_command(ping_cmd.view());

            // If we get here, the server is pingable
            if(!serverPingable.load()) {
                info("Database is now online!");
            }
            serverPingable.store(true);
        }
        catch( const std::exception& e) {

            if (serverPingable.load()) {

                // The server just went offline
                std::string errorMessage = fmt::format("Database now offline. Error pinging the server: {}", e.what());
                error(errorMessage);
            }
            else {

                // We're still down. Throw a trace message just in case.
                std::string errorMessage = fmt::format("Error pinging the server: {}", e.what());
                trace(errorMessage);
            }

            serverPingable.store(false);
        }
        catch( ... ) {
            std::string errorMessage = "Unknown error pinging the server";
            critical(errorMessage);
            serverPingable.store(false);
        }

    }

}