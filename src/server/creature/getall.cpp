
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


    Result<std::vector<creatures::Creature>> Database::getAllCreatures(creatures::SortBy sortBy, bool ascending) {

        (void) ascending;

        info("attempting to get all of the creatures");

        auto creatureList = std::vector<creatures::Creature>{};

        // Start an exception frame
        try {

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

            auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
            if(!collectionResult.isSuccess()) {
                auto error = collectionResult.getError().value();
                std::string errorMessage = fmt::format("database error while listing all of the creatures: {}", error.getMessage());
                warn(errorMessage);
                return Result<std::vector<creatures::Creature>>{error};
            }
            auto collection = collectionResult.getValue().value();
            mongocxx::cursor cursor = collection.find(query_doc.view(), opts);

            for (auto &&doc: cursor) {

                std::string json_str = bsoncxx::to_json(doc);
                debug("Document JSON: {}", json_str);

                // Parse JSON string to nlohmann::json
                nlohmann::json json_doc = nlohmann::json::parse(json_str);

                // Create creature from JSON
                auto creatureResult = creatureFromJson(json_doc);
                if (!creatureResult.isSuccess()) {
                    std::string errorMessage = fmt::format("Unable to parse the JSON in the database to a Creature: {}", creatureResult.getError()->getMessage());
                    warn(errorMessage);
                    continue;
                }

                auto creature = creatureResult.getValue().value();
                creatureList.push_back(creature);

                // Update the cache since we went all the way to the DB to get it
                creatureCache->put(creature.id, creature);
            }

            debug("found {} creatures", creatureList.size());
            return Result<std::vector<creatures::Creature>>{creatureList};

        } catch (const DataFormatException& e) {

            // Log the error
            std::string errorMessage = fmt::format("Data format error while trying to get all of the creatures: {}",
                                                   e.what());
            error(errorMessage);
            return Result<std::vector<creatures::Creature>>{ServerError(ServerError::InternalError, errorMessage)};
        }
        catch (const DatabaseError& e) {
            std::string errorMessage = fmt::format("A database error happened while getting all of the creatures: {}", e.what());
            error(errorMessage);
            return Result<std::vector<creatures::Creature>>{ServerError(ServerError::InternalError, errorMessage)};

        } catch (const std::exception& e) {
            std::string errorMessage = fmt::format("Failed to get all creatures: {}", e.what());
            error(errorMessage);
            return Result<std::vector<creatures::Creature>>{ServerError(ServerError::InternalError, errorMessage)};
        }
        catch (...) {
            std::string errorMessage = "Failed to get all creatures: unknown error";
            error(errorMessage);
            return Result<std::vector<creatures::Creature>>{ServerError(ServerError::InternalError, errorMessage)};
        }

    }

}