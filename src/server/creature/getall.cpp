
#include "server/config.h"

#include <string>
#include <vector>


#include "exception/exception.h"
#include "model/Creature.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "util/cache.h"
#include "util/helpers.h"
#include "util/ObservabilityManager.h" // Include for ObservabilityManager

#include "spdlog/spdlog.h"
#include <fmt/format.h>

#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/find.hpp> // Required for mongocxx::options::find


#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp> // Required for bsoncxx::to_json

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
    extern std::shared_ptr<ObservabilityManager> observability; // Declare observability


    Result<std::vector<creatures::Creature>> Database::getAllCreatures(creatures::SortBy sortBy, bool ascending, const std::shared_ptr<OperationSpan>& parentSpan) { // Pass by const ref

        auto dbSpan = creatures::observability->createChildOperationSpan("Database.getAllCreatures", parentSpan); // Create span

        if (dbSpan) {
            dbSpan->setAttribute("database.collection", CREATURES_COLLECTION);
            dbSpan->setAttribute("database.operation", "find");
            dbSpan->setAttribute("database.system", "mongodb");
            dbSpan->setAttribute("database.name", DB_NAME);
            dbSpan->setAttribute("creatures.sort_by", static_cast<int64_t>(sortBy));
            dbSpan->setAttribute("creatures.sort_ascending", ascending);
        }

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

                default: // Default to sorting by number
                    sort_doc << "number" << 1;
                    debug("defaulting to sorting by number");
                    break;
            }

            // Connect to the database
            auto collectionResult = getCollection(CREATURES_COLLECTION);
            if(!collectionResult.isSuccess()) {
                auto error = collectionResult.getError().value();
                std::string errorMessage = fmt::format("unable to get the creature collection: {}", error.getMessage());
                critical(errorMessage);
                if (dbSpan) {
                    dbSpan->setError(errorMessage);
                    dbSpan->setAttribute("error.type", "DatabaseError");
                    dbSpan->setAttribute("error.code", static_cast<int64_t>(error.getCode()));
                } else {
                    warn("Database span was not created, cannot set error attributes");
                }
                return Result<std::vector<creatures::Creature>>{error};
            }
            auto collection = collectionResult.getValue().value();


            // Query the database with sort options
            auto mongoSpan = creatures::observability->createChildOperationSpan("getAllCreatures::mongo-query", dbSpan);
            mongocxx::options::find opts;
            opts.sort(sort_doc.view());
            mongocxx::cursor cursor = collection.find(query_doc.view(), opts);

            for (auto doc : cursor) {
                auto creatureSpan = creatures::observability->createChildOperationSpan("getAllCreatures::create-creature", mongoSpan);
                json j = json::parse(bsoncxx::to_json(doc));
                auto result = creatureFromJson(j, creatureSpan);
                if(!result.isSuccess()) {
                    auto error = result.getError().value();
                    std::string errorMessage = fmt::format("Data format error while trying to get all of the creatures: {}", error.getMessage());
                    critical(errorMessage);
                    if (creatureSpan) {
                        creatureSpan->setError(errorMessage);
                        creatureSpan->setAttribute("error.type", "DataFormatException");
                        creatureSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
                    } else {
                        warn("Creature span was not created, cannot set error attributes");
                    }
                    return Result<std::vector<creatures::Creature>>{error};
                }
                creatureList.push_back(result.getValue().value());

                // Update the cache as we go
                creatureCache->put(result.getValue().value().id, result.getValue().value());

                creatureSpan->setAttribute("creature.id", result.getValue().value().id);
                creatureSpan->setAttribute("creature.name", result.getValue().value().name);
                creatureSpan->setSuccess();
            }
            mongoSpan->setAttribute("creatures.count", static_cast<int64_t>(creatureList.size()));
            mongoSpan->setSuccess();

            debug("found {} creatures", creatureList.size());
            if (dbSpan) {
                dbSpan->setAttribute("creatures.count", static_cast<int64_t>(creatureList.size()));
                dbSpan->setSuccess();
            } else {
                warn("Database span was not created, cannot set success attributes");
            }
            return Result<std::vector<creatures::Creature>>{creatureList};

        } catch (const DataFormatException& e) {
            // Log the error
            std::string errorMessage = fmt::format("Data format error while trying to get all of the creatures: {}",
                                                   e.what());
            error(errorMessage);
            if (dbSpan) {
                dbSpan->recordException(e);
                dbSpan->setAttribute("error.type", "DataFormatException");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
                dbSpan->setError(errorMessage);
            }
            return Result<std::vector<creatures::Creature>>{ServerError(ServerError::InternalError, errorMessage)};
        }
        catch (const DatabaseError& e) {
            std::string errorMessage = fmt::format("A database error happened while getting all of the creatures: {}", e.what());
            error(errorMessage);
            if (dbSpan) {
                dbSpan->recordException(e);
                dbSpan->setAttribute("error.type", "DatabaseError");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
                dbSpan->setError(errorMessage);
            }
            return Result<std::vector<creatures::Creature>>{ServerError(ServerError::InternalError, errorMessage)};

        } catch (const std::exception& e) {
            std::string errorMessage = fmt::format("Failed to get all creatures: {}", e.what());
            error(errorMessage);
            if (dbSpan) {
                dbSpan->recordException(e);
                dbSpan->setAttribute("error.type", "std::exception");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
                dbSpan->setError(errorMessage);
            }
            return Result<std::vector<creatures::Creature>>{ServerError(ServerError::InternalError, errorMessage)};
        }
        catch (...) {
            std::string errorMessage = "Failed to get all creatures: unknown error";
            error(errorMessage);
            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "UnknownError");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
            }
            return Result<std::vector<creatures::Creature>>{ServerError(ServerError::InternalError, errorMessage)};
        }
    }
}