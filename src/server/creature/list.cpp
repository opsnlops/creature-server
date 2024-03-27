
#include "server/config.h"

#include "spdlog/spdlog.h"

#include "server.pb.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "exception/exception.h"

#include <grpcpp/grpcpp.h>

#include <mongocxx/client.hpp>
#include <bsoncxx/builder/stream/document.hpp>

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;

    grpc::Status CreatureServerImpl::ListCreatures(ServerContext *context, const CreatureFilter *filter,
                                             ListCreaturesResponse *response) {
        debug("calling listCreatures()");

        try {
            db->listCreatures(filter, response);
            debug("got all creatures");
            return {grpc::StatusCode::OK, "✅ Got a list of all of the creatures successfully!"};
        }
        catch (const creatures::DatabaseError &e) {
            error("Database error while getting all creatures: {}", e.what());
            return {grpc::StatusCode::INTERNAL, e.what()};
        }
        catch (const creatures::InternalError &e) {
            error("Internal error while getting all creatures: {}", e.what());
            return {grpc::StatusCode::INTERNAL, e.what()};
        }
        catch (...) {
            error("Unknown error while getting all creatures");
            return {grpc::StatusCode::INTERNAL, "Unknown error"};
        }
    }

    void Database::listCreatures(const CreatureFilter *filter, ListCreaturesResponse *creatureList) {

        info("getting the list of creatures");

        mongocxx::collection collection;
        try {
            collection = getCollection(CREATURES_COLLECTION);
            trace("collection obtained");
        }
        catch (const std::exception &e) {
            std::string errorMessage = fmt::format("Internal error while getting the collection for getting all creatures: {}", e.what());
            error(errorMessage);
            throw creatures::DatabaseError(errorMessage);
        }
        catch (...) {
            std::string errorMessage = "Unknown error getting the collection for while getting all creatures";
            error(errorMessage);
            throw creatures::DatabaseError(errorMessage);
        }

        try {

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
        }
        catch (const creatures::DataFormatException &e) {
            std::string errorMessage = fmt::format("Failed to get all creatures: {}", e.what());
            error(errorMessage);
            throw creatures::DataFormatException(errorMessage);
        }
        catch (const std::exception &e) {
            std::string errorMessage = fmt::format("Internal error while building the document result for getting all creatures: {}", e.what());
            error(errorMessage);
            throw creatures::InternalError(errorMessage);
        }
        catch (...) {
            std::string errorMessage = "Unknown error while building the document result for getting all creatures";
            error(errorMessage);
            throw creatures::InternalError(errorMessage);
        }

    }

}