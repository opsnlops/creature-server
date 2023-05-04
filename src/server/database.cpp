
#include "config.h"

#include <string>
#include <sstream>

#include "spdlog/spdlog.h"

#include "messaging/server.pb.h"
#include "server/database.h"

#include <grpcpp/grpcpp.h>

#include <bsoncxx/builder/stream/document.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/pool.hpp>


using server::Creature;
using server::CreatureName;

using spdlog::info;
using spdlog::debug;
using spdlog::error;
using spdlog::critical;
using spdlog::trace;

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
        auto client = pool.acquire();
        auto collection = (*client)[DB_NAME][collectionName];

        return collection;

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