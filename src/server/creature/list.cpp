
#include "server/config.h"

#include "spdlog/spdlog.h"

#include "messaging/server.pb.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "exception/exception.h"

#include <grpcpp/grpcpp.h>

#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>

#include <bsoncxx/builder/stream/document.hpp>

using server::Creature;
using server::CreatureName;

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;

    grpc::Status CreatureServerImpl::ListCreatures(ServerContext *context, const CreatureFilter *filter,
                                             ListCreaturesResponse *response) {
        debug("calling listCreatures()");

        grpc::Status status;

        db->listCreatures(filter, response);
        status = grpc::Status(grpc::StatusCode::OK,
                              fmt::format("✅🦖Returned all creatures IDs and names"));
        return status;
    }

    grpc::Status Database::listCreatures(const CreatureFilter *filter, ListCreaturesResponse *creatureList) {

        grpc::Status status;
        info("getting the list of creatures");

        auto collection = getCollection(COLLECTION_NAME);
        trace("collection obtained");

        document query_doc{};
        document projection_doc{};
        document sort_doc{};

        switch (filter->sortby()) {
            case server::SortBy::number:
                sort_doc << "number" << 1;
                debug("sorting by number");
                break;

                // Default is by name
            default:
                sort_doc << "name" << 1;
                debug("sorting by name");
                break;
        }

        // We only want the name and _id
        projection_doc << "_id" << 1 << "name" << 1;

        mongocxx::options::find opts{};
        opts.projection(projection_doc.view());
        opts.sort(sort_doc.view());
        mongocxx::cursor cursor = collection.find(query_doc.view(), opts);

        for (auto &&doc: cursor) {

            auto creatureId = creatureList->add_creaturesids();
            creatureIdentifierFromBson(doc, creatureId);

            debug("loaded {}", creatureId->name());

        }

        status = grpc::Status::OK;

        return status;
    }

}