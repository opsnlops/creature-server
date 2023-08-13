
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

#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<Database> db;


    Status CreatureServerImpl::UpdatePlaylist(grpc::ServerContext *context, const server::Playlist *playlist,
                                              server::DatabaseInfo *reply) {

        grpc::Status status;

        debug("trying to update a playlist");

        try {
            db->updatePlaylist(playlist);
            status = grpc::Status(grpc::StatusCode::OK,
                                  "🎉 Playlist updated in database!",
                                  fmt::format("Name: {}, Number of Items: {}",
                                              playlist->name(),
                                              playlist->items_size()));
            reply->set_message(fmt::format("🎉 Playlist updated in database!"));
        }
        catch(const InvalidArgumentException &e) {
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                  "A playlist with an empty id was sent to updateAnimation()",
                                  fmt::format("⛔️️ A playlist ID must be supplied"));
            reply->set_message(fmt::format("⛔️ A playlist ID must be supplied"));
            reply->set_help(fmt::format("playlist's ID cannot be empty"));
        }
        catch(const NotFoundException &e) {
            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                  fmt::format("⚠️ No playlist with ID '{}' found", bsoncxx::oid(playlist->_id()._id()).to_string()),
                                  "Try another ID! 😅");
            reply->set_message(fmt::format("⚠️ No playlist with ID '{}' found", bsoncxx::oid(playlist->_id()._id()).to_string()));
            reply->set_help("Try another ID! 😅");
        }
        catch(const DataFormatException &e) {
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  "Unable to encode request into BSON",
                                  e.what());
            reply->set_message("Unable to encode playlist into BSON");
            reply->set_help(e.what());
        }
        catch(const InternalError &e) {
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  fmt::format("MongoDB error while updating a playlist: {}", e.what()),
                                  e.what());
            reply->set_message("MongoDB error while updating a playlist");
            reply->set_help(e.what());
        }
        catch( ... ) {
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  "🚨 An unknown error happened while updating a playlist 🚨",
                                  "Default catch hit?");
            reply->set_message("Unknown error while updating a playlist");
            reply->set_help("Default catch hit. How did this happen? 🤔");
        }

        return status;
    }


    void Database::updatePlaylist(const server::Playlist *playlist) {

        debug("attempting to update a playlist in the database");

        // Error checking
        if (playlist->_id()._id().empty()) {
            error("a playlist with an empty id was passed into updatePlaylist()");
            throw InvalidArgumentException("a playlist with an empty id was passed into updatePlaylist()");
        }

        auto collection = getCollection(PLAYLISTS_COLLECTION);
        trace("collection obtained");

        try {

            // Convert the animationId to a proper oid
            bsoncxx::oid playlistId = bsoncxx::oid(playlist->_id()._id().data(), bsoncxx::oid::k_oid_length);

            // Create a filter for just this one animation
            bsoncxx::builder::stream::document filter_builder{};
            filter_builder << "_id" << playlistId;

            auto doc_view = playlistToBson(playlist, playlistId);
            auto result = collection.replace_one(filter_builder.view(), doc_view.view());

            if (result) {
                if (result->matched_count() > 1) {

                    // Whoa, this should never happen
                    std::string errorMessage = fmt::format(
                            "more than one document updated at once in updatePlaylist()!! Count: {}",
                            result->matched_count());
                    critical(errorMessage);
                    throw InternalError(errorMessage);

                } else if (result->matched_count() == 0) {

                    // We didn't update anything
                    std::string errorMessage = fmt::format("Update to update playlist. Reason: playlist {} not found",
                                                           playlistId.to_string());
                    info(errorMessage);
                    throw NotFoundException(errorMessage);
                }

                // Hooray, we only did one. :)
                info("✅ playlist {} updated", playlistId.to_string());
                return;

            }

            // Something went wrong
            std::string errorMessage = fmt::format("Unknown errors while updating playlist {} (result wasn't)",
                                                   playlistId.to_string());
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