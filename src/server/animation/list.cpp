
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


    /**
     * List animations in the database for a given creature type
     *
     * @param sortBy How to sort the list (currently unused)
     * @return the status of this request
     */
    Result<std::vector<creatures::AnimationMetadata>> Database::listAnimations(creatures::SortBy sortBy) {

        debug("attempting to list all of the animations");

        (void)sortBy;

        std::vector<creatures::AnimationMetadata> animations;

        try {
            document query_doc{};
            document projection_doc{};
            document sort_doc{};

            // We only want to sort by name at this point
            sort_doc << "metadata.title" << 1;

            // Don't return the track data. Otherwise we'd be loading most of the
            // entire collection into memory just to get a list!
            projection_doc << "tracks" << 0;

            mongocxx::options::find findOptions{};
            findOptions.projection(projection_doc.view());
            findOptions.sort(sort_doc.view());

            auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
            if(!collectionResult.isSuccess()) {
                auto error = collectionResult.getError().value();
                std::string errorMessage = fmt::format("database error while listing all of the animations: {}", error.getMessage());
                warn(errorMessage);
                return Result<std::vector<creatures::AnimationMetadata>>{error};
            }
            auto collection = collectionResult.getValue().value();

            mongocxx::cursor cursor = collection.find(query_doc.view(), findOptions);

            // Go Mongo, go! 🎉
            for (auto &&doc: cursor) {


                std::string json_str = bsoncxx::to_json(doc);
                trace("Document JSON out of Mongo: {}", json_str);

                // Parse JSON string to nlohmann::json
                nlohmann::json json_doc = nlohmann::json::parse(json_str);

                // Create the animation from JSON
                auto animationResult = animationMetadataFromJson(json_doc["metadata"]);
                if (!animationResult.isSuccess()) {
                    std::string errorMessage = fmt::format("Unable to parse the JSON in the database to an AnimationMetadata: {}",
                                                           animationResult.getError()->getMessage());
                    warn(errorMessage);
                    Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::InvalidData, errorMessage)};
                }

                auto animationMetadata = animationResult.getValue().value();
                animations.push_back(animationMetadata);
                debug("found {}", animationMetadata.title);

            }
        }
        catch(const DataFormatException& e) {
            std::string errorMessage = fmt::format("Failed to get all animations: {}", e.what());
            warn(errorMessage);
            return Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        catch(const mongocxx::exception &e) {
            std::string errorMessage = fmt::format("MongoDB Exception while loading animation: {}", e.what());
            critical(errorMessage);
            return Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::InternalError, errorMessage)};
        }
        catch(const bsoncxx::exception &e) {
            std::string errorMessage = fmt::format("BSON error while attempting to load animations: {}", e.what());
            critical(errorMessage);
            return Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::InternalError, errorMessage)};
        }

        // Return a 404 if nothing as found
        if(animations.empty()) {
            std::string errorMessage = fmt::format("No animations found");
            warn(errorMessage);
            return Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::NotFound, errorMessage)};
        }

        info("done loading the animation list");
        return Result<std::vector<creatures::AnimationMetadata>>{animations};
    }
}