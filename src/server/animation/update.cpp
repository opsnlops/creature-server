
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>


#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>


#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;




namespace creatures {

    extern std::shared_ptr<Database> db;


    Status CreatureServerImpl::UpdateAnimation(ServerContext *context,
                                               const Animation *animation,
                                               DatabaseInfo *reply) {

        grpc::Status status;

        debug("trying to update an animation");

        try {
            db->updateAnimation(animation);
            status = grpc::Status(grpc::StatusCode::OK,
                                  "🎉 Animation updated in database!",
                                  fmt::format("Title: {}, Number of Frames: {}",
                                              animation->metadata().title(),
                                              animation->frames_size()));
            reply->set_message(fmt::format("🎉 Animation updated in database!"));
        }
        catch(const InvalidArgumentException &e) {
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                  "AnimationId was empty on updateAnimation()",
                                  fmt::format("⛔️️ An animation ID must be supplied"));
            reply->set_message(fmt::format("⛔️ An animation ID must be supplied"));
            reply->set_help(fmt::format("animationId cannot be empty"));
        }
        catch(const NotFoundException &e) {
            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                  fmt::format("⚠️ No animation with ID '{}' found", bsoncxx::oid(animation->_id()).to_string()),
                                  "Try another ID! 😅");
            reply->set_message(fmt::format("⚠️ No animation with ID '{}' found", bsoncxx::oid(animation->_id()).to_string()));
            reply->set_help("Try another ID! 😅");
        }
        catch(const DataFormatException &e) {
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  "Unable to encode request into BSON",
                                  e.what());
            reply->set_message("Unable to encode animation into BSON");
            reply->set_help(e.what());
        }
        catch(const InternalError &e) {
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  fmt::format("MongoDB error while updating an animation: {}", e.what()),
                                  e.what());
            reply->set_message("MongoDB error while updating animation");
            reply->set_help(e.what());
        }
        catch( ... ) {
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  "🚨 An unknown error happened while updating animation 🚨",
                                  "Default catch hit?");
            reply->set_message("Unknown error while updating an animation");
            reply->set_help("Default catch hit. How did this happen? 🤔");
        }

        return status;
    }


    void Database::updateAnimation(const server::Animation *animation) {

        debug("attempting to update an animation in the database");

        // Error checking
        if (animation->_id().empty()) {
            error("an empty animationId was passed into updateAnimation()");
            throw InvalidArgumentException("an empty animationId was passed into updateAnimation()");
        }

        auto collection = getCollection(ANIMATIONS_COLLECTION);
        trace("collection obtained");

        try {

            // Convert the animationId to a proper oid
            bsoncxx::oid animationId = bsoncxx::oid(animation->_id().data(), bsoncxx::oid::k_oid_length);

            // Create a filter for just this one animation
            bsoncxx::builder::stream::document filter_builder{};
            filter_builder << "_id" << animationId;

            auto doc_view = animationToBson(animation, animationId);
            auto result = collection.replace_one(filter_builder.view(), doc_view.view());

            if (result) {
                if (result->matched_count() > 1) {

                    // Whoa, this should never happen
                    std::string errorMessage = fmt::format(
                            "more than one document updated at once in updateAnimation()!! Count: {}",
                            result->matched_count());
                    critical(errorMessage);
                    throw InternalError(errorMessage);

                } else if (result->matched_count() == 0) {

                    // We didn't update anything
                    std::string errorMessage = fmt::format("Update to update animation. Reason: animation {} not found",
                                                           animationId.to_string());
                    info(errorMessage);
                    throw NotFoundException(errorMessage);
                }

                // Hooray, we only did one. :)
                info("✅ animation {} updated", animationId.to_string());
                return;

            }

            // Something went wrong
            std::string errorMessage = fmt::format("Unknown errors while updating animation {} (result wasn't)",
                                                   animationId.to_string());
            throw InternalError(errorMessage);
        }
        catch (const bsoncxx::exception &e) {
            error("BSON exception while updating an animation: {}", e.what());
            throw DataFormatException(fmt::format("BSON exception while updating an animation: {}", e.what()));
        }
        catch (const mongocxx::exception &e) {
            error("MongoDB exception while updating an animation: {}", e.what());
            throw InternalError(fmt::format("MongoDB exception while updating an animation: {}", e.what()));
        }
    }

}