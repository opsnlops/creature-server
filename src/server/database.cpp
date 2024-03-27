
#include "config.h"

#include <string>
#include <sstream>

#include "spdlog/spdlog.h"

#include "server.pb.h"
#include "server/database.h"

#include <grpcpp/grpcpp.h>

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

    grpc::Status Database::ping() {

        // Ping the database.
        const auto ping_cmd = make_document(kvp("ping", 1));

        auto client = pool.acquire();
        mongocxx::database db = (*client)[DB_NAME];
        db.run_command(ping_cmd.view());

        return grpc::Status::OK;

    }

}