

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
using grpc::Status;

using server::Animation;
using server::Animation_Metadata;
using server::AnimationFilter;
using server::ListAnimationsResponse;

extern creatures::Database *db;

namespace creatures {


    Status CreatureServerImpl::GetAnimation(ServerContext *context, const AnimationId *id, Animation *animation) {

        info("Loading one animation from the database");
        return db->getAnimation(id, animation);
    }


    Status Database::getAnimation(const AnimationId *animationId, Animation *animation) {

        grpc::Status status;
        if (animationId->_id().empty()) {
            error("an empty animationId was passed into getAnimation()");
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                  "AnimationId was empty on getAnimation()",
                                  fmt::format("⛔️️ An animation ID must be supplied"));
            return status;
        }

        auto collection = getCollection(ANIMATIONS_COLLECTION);
        trace("collection gotten");


        try {

            // Convert the ID into an OID
            trace("attempting to convert the ID");
            bsoncxx::oid id = bsoncxx::oid(animationId->_id().data(), 12);
            debug("found animation ID: {}", id.to_string());

            // Create a filter BSON document to match the target document
            auto filter = bsoncxx::builder::stream::document{} << "_id" << id << bsoncxx::builder::stream::finalize;
            trace("filter doc: {}", bsoncxx::to_json(filter));

            // Go try to load it
            bsoncxx::stdx::optional<bsoncxx::document::value> result = collection.find_one(filter.view());

            if (!result) {
                info("🚫 No animation with ID '{}' found", id.to_string());
                status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                      fmt::format("⚠️ No animation with ID '{}' found", id.to_string()),
                                      "Try another ID! 😅");
                return status;
            }

            // Get an owning reference to this doc since it's ours now
            bsoncxx::document::value doc = *result;
            animation->set__id(animationId->_id());
            trace("id loaded");

            // Grab the metadata
            bsoncxx::document::element element = doc["metadata"];
            Animation_Metadata metadata = Animation_Metadata();
            bsonToAnimationMetadata(element.get_document().view(), &metadata);
            *animation->mutable_metadata() = metadata;
            trace("metadata loaded");

            // And load the frames
            populateFramesFromBson(doc, animation);
            trace("frames loaded");

        }
        catch (const bsoncxx::exception &e) {
            error("BSON exception while loading an animation: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  "Unable to encode request into BSON",
                                  e.what());
            return status;
        }
        catch (const mongocxx::exception &e) {
            error("MongoDB exception while loading an animation: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  fmt::format("MongoDB error while loading an animation: {}", e.what()),
                                  e.what());
            return status;
        }


        // Hooray, we loaded it all!
        info("done loading an animation");
        status = grpc::Status(grpc::StatusCode::OK,
                              "Loaded an animation from the database",
                              fmt::format("Title: {}, Number of Frames: {}",
                                          animation->metadata().title(),
                                          animation->frames_size()));
        return status;
    }

}