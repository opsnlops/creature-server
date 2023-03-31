

#include "spdlog/spdlog.h"

#include "messaging/server.pb.h"
#include "server/database.h"

#include <grpcpp/grpcpp.h>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>

using server::Creature;
using server::CreatureName;

using spdlog::info;
using spdlog::debug;
using spdlog::critical;
using spdlog::trace;

using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

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
            db = conn["creature_server"];

            // Ping the database.
            const auto ping_cmd = make_document(kvp("ping", 1));
            db.run_command(ping_cmd.view());
            info("database connection open!");
        }
        catch (const std::exception& e)
        {
            critical("Unable to connect to database: %s", e.what());
        }

    }


    grpc::Status Database::saveCreature(const server::Creature* creature, server::DatabaseInfo* reply) {

        debug("attempting to save a creature in the database");

        auto collection = db["creatures"];
        trace("collection made");


        grpc::Status status;
        trace("status made");

        try {
            auto doc_value = make_document(
                    kvp("name", creature->name()),
                    kvp("poop", "yes")
                    );
            trace("doc_value made");
            db.run_command(doc_value.view());
            trace("run_command done");

            info("save something in the database maybe?");

            status = grpc::Status::OK;
            reply->set_message("saved thingy in the thingy");


        }
        catch (const std::exception& e)
        {

            critical("Unable to connect to database: %s", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL, e.what());
            reply->set_message("well shit");
            reply->set_help(e.what());

        }

        return status;
    }

}