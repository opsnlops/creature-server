
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
    std::vector<creatures::AnimationMetadata> Database::listAnimations(creatures::SortBy sortBy) {

        debug("attempting to list all of the animations");

        std::vector<creatures::AnimationMetadata> animations;

        try {
            auto collection = getCollection(ANIMATIONS_COLLECTION);
            trace("collection obtained");

            document query_doc{};
            document projection_doc{};
            document sort_doc{};

            // We only want to sort by name at this point
            sort_doc << "metadata.title" << 1;

            // Don't return the frame data. Otherwise we'd be loading most of the
            // entire collection into memory just to get a list!
            projection_doc << "frames" << 0;


            mongocxx::options::find findOptions{};
            findOptions.projection(projection_doc.view());
            findOptions.sort(sort_doc.view());

            mongocxx::cursor cursor = collection.find(query_doc.view(), findOptions);

            // Go Mongo, go! 🎉
            for (auto &&doc: cursor) {

                auto animationMetadataDoc = doc["metadata"];
                trace("got the metadata doc");

                auto animationMetadata = animationMetadataFromBson(animationMetadataDoc);
                animations.push_back(animationMetadata);
                debug("found {}", animationMetadata.title);

            }
        }
        catch(const DataFormatException& e) {
            std::string errorMessage = fmt::format("Failed to get all animations: {}", e.what());
            warn(errorMessage);
            throw creatures::DataFormatException(errorMessage);
        }
        catch(const mongocxx::exception &e) {
            std::string errorMessage = fmt::format("MongoDB Exception while loading animation: {}", e.what());
            critical(errorMessage);
            throw creatures::InternalError(errorMessage);
        }
        catch(const bsoncxx::exception &e) {
            std::string errorMessage = fmt::format("BSON error while attempting to load animations: {}", e.what());
            critical(errorMessage);
            throw creatures::InternalError(errorMessage);
        }

        // Return a 404 if nothing as found
        if(animations.empty()) {
            std::string errorMessage = fmt::format("No animations for that creature type found");
            warn(errorMessage);
            throw creatures::NotFoundException(errorMessage);
        }

        info("done loading the animation list");
        return animations;
    }
}