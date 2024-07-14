
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "server/database.h"
#include "server/namespace-stuffs.h"

#include "model/Playlist.h"
#include "model/PlaylistItem.h"
#include "util/Result.h"

namespace creatures {


    Result<creatures::Playlist> Database::playlistFromJson(json playlistJson) {

        debug("attempting to create a playlist from JSON via playlistFromJson(): {}",
              playlistJson.dump(4));

        // Keep track of what we're working on so we can make a good error message
        std::string working_on;

        try {

            auto playlist = Playlist();
            working_on = "id";
            playlist.id = playlistJson[working_on];
            debug("id: {}", playlist.id);

            working_on = "name";
            playlist.name = playlistJson[working_on];
            debug("name: {}", playlist.name);

            working_on = "number_of_items";
            playlist.number_of_items = playlistJson[working_on];
            debug("number_of_items: {}", playlist.number_of_items);

            // Add all the items
            std::vector<json> itemsJson = playlistJson["items"];
            for(const auto& itemJson : itemsJson) {
                auto itemResult = playlistItemFromJson(itemJson);
                if (!itemResult.isSuccess()) {
                    auto error = itemResult.getError();
                    warn("Error while creating a playlistItem from JSON while making a playlist: {}", error->getMessage());
                    return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, error->getMessage())};
                }
                playlist.items.push_back(itemResult.getValue().value());
            }

            return Result<creatures::Playlist>{playlist};
        }
        catch (const nlohmann::json::exception &e) {
            std::string errorMessage = fmt::format("Error while creating a playlist from JSON (field '{}'): {}",
                                                   working_on, e.what());
            warn(errorMessage);
            return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
        }
    }

    Result<creatures::PlaylistItem> Database::playlistItemFromJson(json playlistItemJson) {

        debug("attempting to create a playlistItem from JSON via playlistItemFromJson()");

        // Keep track of the element we're working on so we can have good error messages
        std::string working_on;

        try {

            auto playlistItem = PlaylistItem();
            working_on = "animation_id";
            playlistItem.animation_id = playlistItemJson[working_on];
            debug("animation_id: {}", playlistItem.animation_id);

            working_on = "weight";
            playlistItem.weight = playlistItemJson[working_on];
            debug("weight: {}", playlistItem.weight);

            debug("done with playlistItemFromJson");
            return Result<creatures::PlaylistItem>{playlistItem};
        }
        catch (const nlohmann::json::exception &e) {
            std::string errorMessage = fmt::format("Error while creating a playlistItem from JSON (field '{}'): {}",
                                                   working_on, e.what());
            warn(errorMessage);
            return Result<creatures::PlaylistItem>{ServerError(ServerError::InvalidData, errorMessage)};
        }
    }


}