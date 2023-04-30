
#include <string>

#include "spdlog/spdlog.h"

#include "messaging/server.pb.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "exception/exception.h"

#include <fmt/format.h>

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

    grpc::Status CreatureServerImpl::GetAllCreatures(ServerContext *context, const CreatureFilter *filter,
                                               GetAllCreaturesResponse *response) {

        debug("called handleListCreatures()");
        grpc::Status status;

        db->getAllCreatures(filter, response);
        status = grpc::Status(grpc::StatusCode::OK,
                              fmt::format("🐰🐻🦁 Returned all creatures!"));

        return status;
    }


    grpc::Status Database::getAllCreatures(const CreatureFilter *filter, GetAllCreaturesResponse *creatureList) {
        grpc::Status status;
        info("getting all of the creatures");

        auto collection = getCollection(COLLECTION_NAME);
        trace("collection obtained");

        document query_doc{};
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

        mongocxx::options::find opts{};
        opts.sort(sort_doc.view());
        mongocxx::cursor cursor = collection.find(query_doc.view(), opts);

        for (auto &&doc: cursor) {

            auto creature = creatureList->add_creatures();
            creatureFromBson(doc, creature);

            debug("loaded {}", creature->name());
        }

        status = grpc::Status::OK;
        return status;

    }

}