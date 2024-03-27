
#include "server/config.h"

#include <string>

#include "spdlog/spdlog.h"

#include "server.pb.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "exception/exception.h"

#include <fmt/format.h>

#include <grpcpp/grpcpp.h>

#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>

#include <bsoncxx/builder/stream/document.hpp>

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;

    grpc::Status CreatureServerImpl::GetAllCreatures(ServerContext *context, const CreatureFilter *filter,
                                               GetAllCreaturesResponse *response) {

        debug("called GetAllCreatures()");

        try {
            db->getAllCreatures(filter, response);
            return {grpc::StatusCode::OK, "Got all of the creatures! ✅🦁🦜"};
        }
        catch( const DataFormatException& e) {
            error("Data format exception while getting all creatures: {}", e.what());
            return {grpc::StatusCode::INTERNAL, e.what()};
        }
        catch( const InternalError& e) {
            error("Internal error while getting all creatures: {}", e.what());
            return {grpc::StatusCode::INTERNAL, e.what()};
        }
        catch ( ... ) {
            error("Unknown error while getting all creatures");
            return {grpc::StatusCode::INTERNAL, "Unknown error"};
        }

    }


    void Database::getAllCreatures(const CreatureFilter *filter, GetAllCreaturesResponse *creatureList) {
        grpc::Status status;
        info("attempting to get all of the creatures");

        // Start an exception frame
        try {

            auto collection = getCollection(CREATURES_COLLECTION);
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

            return;

        } catch (const DataFormatException& e) {

                // Log the error
                std::string errorMessage = fmt::format("Data format error while trying to get all of the creatures: {}", e.what());
                error(errorMessage);
                throw creatures::DataFormatException(errorMessage);}
        catch (const DatabaseError& e) {
                std::string errorMessage = fmt::format("A database error happened while getting all of the creatures: {}", e.what());
                error(errorMessage);
                throw creatures::InternalError(errorMessage);

        } catch (const std::exception& e) {
            std::string errorMessage = fmt::format("Failed to get all creatures: {}", e.what());
            error(errorMessage);
            throw creatures::InternalError(errorMessage);
        }
        catch (...) {
            std::string errorMessage = "Failed to get all creatures: unknown error";
            error(errorMessage);
            throw creatures::InternalError(errorMessage);
        }

    }

}