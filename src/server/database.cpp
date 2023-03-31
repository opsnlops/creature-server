

#include "spdlog/spdlog.h"

#include "messaging/server.pb.h"
#include "server/database.h"


#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>

using server::Creature;
using server::CreatureName;

using spdlog::info;
using spdlog::debug;
using spdlog::critical;

namespace creatures {

    Database::Database() {

        info("starting up database connection");

        try {
            // Create an instance.
            mongocxx::instance inst{};

            // Replace the connection string with your MongoDB deployment's connection string.
            const auto uri = mongocxx::uri{DB_URI};

            // Set the version of the Stable API on the client.
            mongocxx::options::client client_options;
            const auto api = mongocxx::options::server_api{mongocxx::options::server_api::version::k_version_1};
            client_options.server_api_opts(api);

            // Setup the connection and get a handle on the "creatures" database.
            mongocxx::client conn{uri, client_options};
            mongocxx::database db = conn["creatures"];

            // Ping the database.
            const auto ping_cmd = bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("ping", 1));
            db.run_command(ping_cmd.view());
            info("database connection open!");
        }
        catch (const std::exception& e)
        {

            critical("Unable to connect to database: %s", e.what());
        }

    }

}