
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>


#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;


namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<ObservabilityManager> observability;


    Result<json> Database::getAnimationJson(animationId_t animationId, std::unique_ptr<OperationSpan> parentSpan) {

        debug("attempting to get the JSON for an animation by ID: {}", animationId);

        auto dbSpan = creatures::observability->createChildOperationSpan("Database.getAnimationJson",
                                    std::move(parentSpan));

        if ( !parentSpan ) {
            warn("no parent span provided for Database.getAnimationJson, creating a root span");
        }

        if (dbSpan) {
            dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
            dbSpan->setAttribute("animation.id", animationId);
        }

        if(animationId.empty()) {

            auto errorMessage = fmt::format("unable to get an animation because the id was empty");
            info(errorMessage);

            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "InvalidData");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            }

            return Result<json>{ServerError(ServerError::InvalidData, "unable to get an animation because the id was empty")};
        }

        try {
            bsoncxx::builder::stream::document filter_builder;
            filter_builder << "id" << animationId;

            // Search for the document
            auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
            if(!collectionResult.isSuccess()) {
                auto error = collectionResult.getError().value();
                std::string errorMessage = fmt::format("Database error while attempting to get an animation by ID: {}", error.getMessage());
                warn(errorMessage);

                if (dbSpan) {
                    dbSpan->setError(errorMessage);
                    dbSpan->setAttribute("error.type", "InvalidData");
                    dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                }

                return Result<json>{error};
            }
            auto collection = collectionResult.getValue().value();
            auto maybe_result = collection.find_one(filter_builder.view());

            // Check if the document was found
            if (maybe_result) {
                // Convert BSON document to JSON using nlohmann::json
                bsoncxx::document::view view = maybe_result->view();
                nlohmann::json json_result = nlohmann::json::parse(bsoncxx::to_json(view));

                if (dbSpan) {
                    dbSpan->setSuccess();
                    dbSpan->setAttribute("json_size", static_cast<int64_t>(json_result.size()));
                }

                return Result<json>{json_result};
            } else {
                std::string errorMessage = fmt::format("no animation id '{}' found", animationId);
                warn(errorMessage);

                if (dbSpan) {
                    dbSpan->setError(errorMessage);
                    dbSpan->setAttribute("error.type", "InternalError");
                    dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
                }

                return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
            }
        } catch (const mongocxx::exception &e) {
            std::string errorMessage = fmt::format("a MongoDB error happened while loading an animation by ID: {}", e.what());
            critical(errorMessage);

            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "InternalError");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
            }

            return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
        } catch ( ... ) {
            std::string errorMessage = fmt::format("An unknown error happened while loading an animation by ID");
            critical(errorMessage);

            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "InternalError");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
            }

            return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
        }

    }


    Result<creatures::Animation> Database::getAnimation(const animationId_t& animationId, std::unique_ptr<OperationSpan> parentSpan) {

        if ( !parentSpan ) {
            warn("no parent span provided for Database.getAnimation, creating a root span");
        }

        auto dbSpan = creatures::observability->createChildOperationSpan("Database.getAnimation",
                            std::move(parentSpan));
        if (dbSpan) {
            dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
            dbSpan->setAttribute("animation.id", animationId);
        }

        if (animationId.empty()) {
            std::string errorMessage = "unable to get an animation because the id was empty";
            warn(errorMessage);

            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "InvalidData");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            }

            return Result<creatures::Animation>{ServerError(ServerError::InvalidData, errorMessage)};
        }


        // Go to the database and get the animation's raw JSON
        auto animationJson = getAnimationJson(animationId, std::move(dbSpan));
        if (!animationJson.isSuccess()) {
            auto error = animationJson.getError().value();
            std::string errorMessage = fmt::format("unable to get an animation by ID: {}",
                                                   animationJson.getError()->getMessage());
            warn(errorMessage);

            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "InvalidData");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            }

            return Result<creatures::Animation>{error};
        }

        // Covert it to an Animation object (if possible)
        auto result = animationFromJson(animationJson.getValue().value());
        if (!result.isSuccess()) {
            auto error = result.getError().value();
            std::string errorMessage = fmt::format("unable to get an animation by ID: {}",
                                                   result.getError()->getMessage());
            warn(errorMessage);

            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "InvalidData");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            }

            return Result<creatures::Animation>{error};
        }

        // Create the animation
        auto animation = result.getValue().value();

        if (dbSpan) {
            dbSpan->setSuccess();
            dbSpan->setAttribute("animation.title", animation.metadata.title);
        }

        return Result<creatures::Animation>{animation};

    }
}