
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>


#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;


namespace creatures {

    extern std::shared_ptr<Database> db;

    Result<json> Database::getPlaylistJson(playlistId_t playlistId) {

        debug("attempting to get the JSON for a playlist by ID: {}", playlistId);

        if(playlistId.empty()) {
            info("an empty playlistId was passed into getPlaylistJson()");
            return Result<json>{ServerError(ServerError::InvalidData, "unable to get a playlist because the id was empty")};
        }

        try {
            bsoncxx::builder::stream::document filter_builder;
            filter_builder << "id" << playlistId;

            // Search for the document
            auto collection = getCollection(PLAYLISTS_COLLECTION);
            auto maybe_result = collection.find_one(filter_builder.view());

            // Check if the document was found
            if (maybe_result) {
                // Convert BSON document to JSON using nlohmann::json
                bsoncxx::document::view view = maybe_result->view();
                nlohmann::json json_result = nlohmann::json::parse(bsoncxx::to_json(view));
                return Result<json>{json_result};
            } else {
                std::string errorMessage = fmt::format("no playlist id '{}' found", playlistId);
                warn(errorMessage);
                return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
            }
        } catch (const mongocxx::exception &e) {
            std::string errorMessage = fmt::format("a MongoDB error happened while loading a playlist by ID: {}", e.what());
            critical(errorMessage);
            return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
        } catch ( ... ) {
            std::string errorMessage = fmt::format("An unknown error happened while loading a playlist by ID");
            critical(errorMessage);
            return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
        }

    }


    Result<creatures::Playlist> Database::getPlaylist(const playlistId_t& playlistId) {

        if (playlistId.empty()) {
            std::string errorMessage = "unable to get a playlist because the id was empty";
            warn(errorMessage);
            return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
        }


        // Go to the database and get the playlist's raw JSON
        auto playlistJson = getPlaylistJson(playlistId);
        if (!playlistJson.isSuccess()) {
            auto error = playlistJson.getError().value();
            std::string errorMessage = fmt::format("unable to get a playlist by ID: {}",
                                                   playlistJson.getError()->getMessage());
            warn(errorMessage);
            return Result<creatures::Playlist>{error};
        }

        // Covert it to an Playlist object (if possible)
        auto result = playlistFromJson(playlistJson.getValue().value());
        if (!result.isSuccess()) {
            auto error = result.getError().value();
            std::string errorMessage = fmt::format("unable to get a playlist by ID: {}",
                                                   result.getError()->getMessage());
            warn(errorMessage);
            return Result<creatures::Playlist>{error};
        }

        // Create the playlist
        auto playlist = result.getValue().value();
        return Result<creatures::Playlist>{playlist};

    }
}