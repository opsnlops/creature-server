
#include "server/config.h"

#include <string>
#include <vector>


#include "exception/exception.h"
#include "model/Creature.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "util/cache.h"
#include "util/helpers.h"

#include "spdlog/spdlog.h"
#include <fmt/format.h>

#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>

#include <bsoncxx/builder/stream/document.hpp>

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;


    std::vector<creatures::Creature> Database::getAllCreatures(creatures::SortBy sortBy, bool ascending) {

        info("attempting to get all of the creatures");

        auto creatureList = std::vector<creatures::Creature>{};

        // Start an exception frame
        try {

            auto collection = getCollection(CREATURES_COLLECTION);
            trace("collection obtained");

            document query_doc{};
            document sort_doc{};

            switch (sortBy) {
                case SortBy::number:
                    sort_doc << "number" << 1;
                    debug("sorting by number");
                    break;

                case SortBy::name:
                    sort_doc << "name" << 1;
                    debug("sorting by name");
                    break;

                    // Default is by name
                default:
                    sort_doc << "name" << 1;
                    debug("sorting by name (as default)");
                    break;
            }

            mongocxx::options::find opts{};
            opts.sort(sort_doc.view());
            mongocxx::cursor cursor = collection.find(query_doc.view(), opts);

            for (auto &&doc: cursor) {

                auto creature = creatureFromBson(doc);
                creatureList.push_back(creature);

                // Update the cache since we went all the way to the DB to get it
                creatureCache->put(creature.id, creature);
            }
            debug("found {} creatures", creatureList.size());

            return creatureList;

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