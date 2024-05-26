
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>


#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>


#include "model/Animation.h"
#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"

#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<Database> db;


//    void Database::updateAnimation(const creatures::Animation animation) {
//
//        debug("attempting to update an animation in the database");
//
//        // Error checking
//        if (animation.id.empty()) {
//            error("an empty animationId was passed into updateAnimation()");
//            throw InvalidArgumentException("an empty animationId was passed into updateAnimation()");
//        }
//
//        auto collection = getCollection(ANIMATIONS_COLLECTION);
//        trace("collection obtained");
//
//        try {
//
//            // Convert the animationId to a proper oid
//            bsoncxx::oid animationId = bsoncxx::oid(animation->_id().data(), bsoncxx::oid::k_oid_length);
//
//            // Create a filter for just this one animation
//            bsoncxx::builder::stream::document filter_builder{};
//            filter_builder << "_id" << animationId;
//
//            auto doc_view = animationToBson(animation, animationId);
//            auto result = collection.replace_one(filter_builder.view(), doc_view.view());
//
//            if (result) {
//                if (result->matched_count() > 1) {
//
//                    // Whoa, this should never happen
//                    std::string errorMessage = fmt::format(
//                            "more than one document updated at once in updateAnimation()!! Count: {}",
//                            result->matched_count());
//                    critical(errorMessage);
//                    throw InternalError(errorMessage);
//
//                } else if (result->matched_count() == 0) {
//
//                    // We didn't update anything
//                    std::string errorMessage = fmt::format("Update to update animation. Reason: animation {} not found",
//                                                           animationId.to_string());
//                    info(errorMessage);
//                    throw NotFoundException(errorMessage);
//                }
//
//                // Hooray, we only did one. :)
//                info("✅ animation {} updated", animationId.to_string());
//                return;
//
//            }
//
//            // Something went wrong
//            std::string errorMessage = fmt::format("Unknown errors while updating animation {} (result wasn't)",
//                                                   animationId.to_string());
//            throw InternalError(errorMessage);
//        }
//        catch (const bsoncxx::exception &e) {
//            error("BSON exception while updating an animation: {}", e.what());
//            throw DataFormatException(fmt::format("BSON exception while updating an animation: {}", e.what()));
//        }
//        catch (const mongocxx::exception &e) {
//            error("MongoDB exception while updating an animation: {}", e.what());
//            throw InternalError(fmt::format("MongoDB exception while updating an animation: {}", e.what()));
//        }
//    }

}