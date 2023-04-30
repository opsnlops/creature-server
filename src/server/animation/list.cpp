
#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>


#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"

#include "server/animation/animation.h"

using bsoncxx::builder::stream::document;

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

using grpc::ServerContext;

using server::Animation;
using server::Animation_Metadata;
using server::AnimationFilter;
using server::ListAnimationsResponse;

namespace creatures {

    extern std::shared_ptr<Database> db;

    Status CreatureServerImpl::ListAnimations(ServerContext *context,
                                              const AnimationFilter *request,
                                              ListAnimationsResponse *response) {

        info("Listing the animations in the database");
        return db->listAnimations(request, response);
    }

    /**
     * List animations in the database for a given creature type
     *
     * @param filter what CreatureType to look for
     * @param animationList the list to fill out
     * @return the status of this request
     */
    grpc::Status Database::listAnimations(const AnimationFilter *filter, ListAnimationsResponse *animationList) {

        trace("attempting to list all of the animation for a filter ({})", filter->type());

        grpc::Status status;

        uint32_t numberOfAnimationsFound = 0;

        try {
            auto collection = getCollection(ANIMATIONS_COLLECTION);
            trace("collection obtained");

            document query_doc{};
            document projection_doc{};
            document sort_doc{};

            // We only want to sort by name at this point
            sort_doc << "metadata.title" << 1;

            // We want the idea and metadata only (no frames)
            projection_doc << "_id" << 1 << "metadata" << 1;

            // We only want documents of the given creature type
            query_doc << "metadata.creature_type" << bsoncxx::types::b_int32{static_cast<int32_t>(filter->type())};

            mongocxx::options::find findOptions{};
            findOptions.projection(projection_doc.view());
            findOptions.sort(sort_doc.view());

            mongocxx::cursor cursor = collection.find(query_doc.view(), findOptions);

            // Go Mongo, go! 🎉
            for (auto &&doc: cursor) {

                auto animationId = animationList->add_animations();

                // Extract the ID
                bsoncxx::document::element element = doc["_id"];
                if (element && element.type() != bsoncxx::type::k_oid) {
                    error("Field `_id` was not an OID in the database");
                    throw DataFormatException("Field '_id' was not a bsoncxx::oid in the database");
                }
                const bsoncxx::oid &oid = element.get_oid().value;
                const char *oid_data = oid.bytes();
                animationId->set__id(oid_data, bsoncxx::oid::k_oid_length);
                trace("set the _id to {}", oid.to_string());

                // Grab the metadata
                element = doc["metadata"];

                Animation_Metadata metadata = Animation_Metadata();

                bsonToAnimationMetadata(element.get_document().view(), &metadata);
                *animationId->mutable_metadata() = metadata;

                debug("loaded {}", animationId->metadata().title());
                numberOfAnimationsFound++;
            }
        }
        catch(const DataFormatException &e) {
            warn("DataFormatException while getting animations: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  fmt::format("🚨 Server-side error while getting animations: {}", e.what()));
            return status;
        }
        catch(const mongocxx::exception &e) {
            critical("MongoDB error while attempting to load animations: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  fmt::format("🚨 MongoDB error while attempting to load animations: {}", e.what()));
            return status;
        }
        catch(const bsoncxx::exception &e) {
            critical("BSON error while attempting to load animations: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  fmt::format("🚨 BSON error while attempting to load animations: {}", e.what()));
            return status;
        }

        // Return a 404 if nothing as found
        if(numberOfAnimationsFound == 0) {
            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                  "🚫 No animations for that creature type found");
            return status;
        }

        // If we made this far, we're good! 😍
        status = grpc::Status(grpc::StatusCode::OK,
                              fmt::format("✅ Found {} animations", numberOfAnimationsFound));
        return status;
    }
}