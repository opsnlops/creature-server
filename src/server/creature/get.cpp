
#include "server/config.h"

#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fmt/format.h>


#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <bsoncxx/builder/stream/document.hpp>


#include "exception/exception.h"
#include "model/Creature.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "util/cache.h"
#include "util/helpers.h"
#include "util/Result.h"
#include "util/ObservabilityManager.h" // Include for ObservabilityManager


#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::stream::finalize; // Added for finalize
using json = nlohmann::json;

namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
    extern std::shared_ptr<ObservabilityManager> observability;


    Result<json> Database::getCreatureJson(creatureId_t creatureId, const std::shared_ptr<OperationSpan>& parentSpan) { // Pass by const ref
        debug("attempting to get a creature's JSON by ID: {}", creatureId);

        auto dbSpan = creatures::observability->createChildOperationSpan("Database.getCreatureJson", parentSpan); // Create span

        if (!parentSpan) {
            warn("no parent span provided for Database.getCreatureJson, creating a root span");
        }

        if (dbSpan) {
            dbSpan->setAttribute("database.collection", CREATURES_COLLECTION);
            dbSpan->setAttribute("database.operation", "find_one");
            dbSpan->setAttribute("creature.id", creatureId);
            dbSpan->setAttribute("database.system", "mongodb");
            dbSpan->setAttribute("database.name", DB_NAME);
        }

        if(creatureId.empty()) {
            info("an empty creatureID was passed into getCreatureJson()");
            if (dbSpan) { // Error handling for span
                dbSpan->setError("empty creatureID");
                dbSpan->setAttribute("error.type", "InvalidData");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            }
            return Result<json>{ServerError(ServerError::InvalidData, "unable to get a creature because the id was empty")};
        }

        // Connect to the database
        auto collectionResult = getCollection(CREATURES_COLLECTION);
        if(!collectionResult.isSuccess()) {
            auto error = collectionResult.getError().value();
            std::string errorMessage = fmt::format("unable to get the creature collection: {}", error.getMessage());
            critical(errorMessage);
            if (dbSpan) { // Error handling for span
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "DatabaseError");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(error.getCode()));
            }
            return Result<json>{error};
        }
        auto collection = collectionResult.getValue().value();
        debug("collection obtained");

        // Query the database
        std::shared_ptr<OperationSpan> mongoSpan;
        try {
            mongoSpan = creatures::observability->createChildOperationSpan("getCreatureJson.mongoQuery", dbSpan);

            auto query = document{} << "id" << creatureId << finalize;
            std::optional<bsoncxx::document::value> maybe_result;

            debug("executing query for creature ID: {}", creatureId);
            maybe_result = collection.find_one(query.view());
            debug("query executed for creature ID: {}", creatureId);

            mongoSpan->setSuccess();
            mongoSpan->setAttribute("db.response_size_bytes", static_cast<int64_t>(maybe_result ? bsoncxx::to_json(maybe_result->view()).length() : 0));

            if(!maybe_result) {
                std::string errorMessage = fmt::format("Creature not found: {}", creatureId);
                warn(errorMessage);
                if (dbSpan) { // Error handling for span
                    dbSpan->setError(errorMessage);
                    dbSpan->setAttribute("error.type", "NotFound");
                    dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::NotFound));
                } else {
                    warn("Database span was not created, cannot set error attributes");
                }
                return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
            }

            // Convert the result to JSON
            auto convertToJsonSpan = creatures::observability->createChildOperationSpan("getCreatureJson.json::parse", dbSpan);
            json j = json::parse(bsoncxx::to_json(maybe_result->view()));
            if (convertToJsonSpan) {
                convertToJsonSpan->setSuccess();
                convertToJsonSpan->setAttribute("json.size_bytes", static_cast<int64_t>(j.dump().length()));
            } else {
                warn("JSON conversion span was not created, cannot set success attributes");
            }

            if (dbSpan) { // Success for span
                dbSpan->setAttribute("db.response_size_bytes", static_cast<int64_t>(bsoncxx::to_json(maybe_result->view()).length()));
                dbSpan->setSuccess();
            }
            return Result<json>{j};

        } catch (const mongocxx::exception& e) {
            std::string errorMessage = fmt::format("MongoDB exception caught while finding creature {}: {}", creatureId, e.what());
            critical(errorMessage);
            if (mongoSpan) {
                mongoSpan->recordException(e);
                mongoSpan->setAttribute("error.message", e.what());
                mongoSpan->setAttribute("error.type", "MongoDBException");
                mongoSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::DatabaseError));
            } else {
                warn("MongoDB span was not created, cannot set error attributes");
            }
            return Result<json>{ServerError(ServerError::DatabaseError, errorMessage)};
        } catch (const std::exception& e) {
            std::string errorMessage = fmt::format("Standard exception caught while finding creature {}: {}", creatureId, e.what());
            critical(errorMessage);
            if (mongoSpan) {
                mongoSpan->recordException(e);
                mongoSpan->setAttribute("error.type", "StandardException");
                mongoSpan->setAttribute("error.message", e.what());
                mongoSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
            } else {
                warn("Standard exception caught, cannot set error attributes on mongoSpan");
            }
            return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
        } catch (...) {
            std::string errorMessage = fmt::format("Unknown exception caught while finding creature {}", creatureId);
            critical(errorMessage);
            if (mongoSpan) {
                mongoSpan->setError(errorMessage);
                mongoSpan->setAttribute("error.type", "UnknownException");
                mongoSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
            } else  {
                warn("Unknown exception caught, cannot set error attributes on mongoSpan");
            }
            return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
        }
    }


    /**
     * Look up a creature by it's ID
     *
     * @param creatureId the ID of the creature
     * @return the Creature object that was found, or a ServerError if it couldn't be found or looked up
     *
     */
    Result<creatures::Creature> Database::getCreature(const creatureId_t& creatureId, const std::shared_ptr<OperationSpan>& parentSpan) { // Pass by const ref

        auto dbSpan = creatures::observability->createChildOperationSpan("Database.getCreature", parentSpan); // Create span

        if (dbSpan) {
            dbSpan->setAttribute("creature.id", creatureId);
        }


        if (creatureId.empty()) {
            std::string errorMessage = "unable to get a creature because the id was empty";
            warn(errorMessage);
            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "InvalidData");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            } else {
                warn("Database span was not created, cannot set error attributes");
            }
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }

        // Go to the database and get the creature
        auto jsonSpan = creatures::observability->createChildOperationSpan("getCreature.getCreatureJson", dbSpan);
        auto creatureJson = getCreatureJson(creatureId, jsonSpan); // Pass span
        if(!creatureJson.isSuccess()) {
            auto error = creatureJson.getError().value();
            std::string errorMessage = fmt::format("unable to get a creature by ID: {}", creatureJson.getError()->getMessage());
            warn(errorMessage);
            if (jsonSpan) { // Error handling for span
                jsonSpan->setError(errorMessage);
                jsonSpan->setAttribute("error.type", "DatabaseError");
                jsonSpan->setAttribute("error.code", static_cast<int64_t>(error.getCode()));
            } else {
                warn("JSON span was not created, cannot set error attributes");
            }
            return Result<creatures::Creature>{error};
        }
        jsonSpan->setSuccess();
        jsonSpan->setAttribute("json.size_bytes", static_cast<int64_t>(creatureJson.getValue().value().dump().length()));

        // Covert it to our Creature object if we can
        auto fetchSpan = creatures::observability->createChildOperationSpan("getCreature.creatureFromJson", dbSpan);
        auto result = creatureFromJson(creatureJson.getValue().value(), fetchSpan);
        if(!result.isSuccess()) {
            auto error = result.getError().value();
            std::string errorMessage = fmt::format("unable to get a creature by ID: {}", result.getError()->getMessage());
            warn(errorMessage);
            if (fetchSpan) { // Error handling for span
                fetchSpan->setError(errorMessage);
                fetchSpan->setAttribute("error.type", "DataFormatException");
                fetchSpan->setAttribute("error.code", static_cast<int64_t>(error.getCode()));
            }
            return Result<creatures::Creature>{error};
        }
        fetchSpan->setSuccess();
        fetchSpan->setAttribute("creature.name", result.getValue().value().name);

        // Create the creature
        auto creature = result.getValue().value();

        // As long as we're here, let's update the cache
        creatureCache->put(creatureId, creature);
        if (dbSpan) { // Success for span
            dbSpan->setAttribute("cache.status", "updated");
            dbSpan->setSuccess();
        }
        return Result<creatures::Creature>{creature};
    }
}