
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "server/database.h"
#include "server/namespace-stuffs.h"

#include "model/Playlist.h"
#include "model/PlaylistItem.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"



namespace creatures {

    extern std::shared_ptr<ObservabilityManager> observability;

    Result<creatures::Playlist> Database::playlistFromJson(json playlistJson, std::shared_ptr<OperationSpan> parentSpan) {

        if(!parentSpan) {
            parentSpan = observability->createOperationSpan("get_playlist_from_json_operation");
        }
        auto span = observability->createChildOperationSpan("get_playlist_from_json", parentSpan);

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
                auto itemResult = playlistItemFromJson(itemJson, span);
                if (!itemResult.isSuccess()) {
                    auto error = itemResult.getError();
                    warn("Error while creating a playlistItem from JSON while making a playlist: {}", error->getMessage());
                    return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, error->getMessage())};
                }
                playlist.items.push_back(itemResult.getValue().value());
            }

            span->setSuccess();
            return Result<creatures::Playlist>{playlist};
        }
        catch (const nlohmann::json::exception &e) {
            std::string errorMessage = fmt::format("Error while creating a playlist from JSON (field '{}'): {} (object: {})",
                                                   working_on, e.what(), playlistJson.dump(4));
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
        }
    }

    Result<creatures::PlaylistItem> Database::playlistItemFromJson(json playlistItemJson, std::shared_ptr<OperationSpan> parentSpan) {

        if(!parentSpan) {
            parentSpan = observability->createOperationSpan("get_playlist_item_from_json_operation");
        }
        auto span = observability->createChildOperationSpan("get_playlist_item_from_json", parentSpan);

        debug("attempting to create a playlistItem from JSON via playlistItemFromJson()");

        // Keep track of the element we're working on so we can have good error messages
        std::string working_on;

        try {

            auto playlistItem = PlaylistItem();
            working_on = "animation_id";
            playlistItem.animation_id = playlistItemJson[working_on];
            debug("animation_id: {}", playlistItem.animation_id);
            span->setAttribute("animation_id", playlistItem.animation_id);

            working_on = "weight";
            playlistItem.weight = playlistItemJson[working_on];
            debug("weight: {}", playlistItem.weight);
            span->setAttribute("weight", playlistItem.weight);

            debug("done with playlistItemFromJson");

            span->setSuccess();
            return Result<creatures::PlaylistItem>{playlistItem};
        }
        catch (const nlohmann::json::exception &e) {
            std::string errorMessage = fmt::format("Error while creating a playlistItem from JSON (field '{}'): {}",
                                                   working_on, e.what());
            warn(errorMessage);
            span->recordException(e);
            return Result<creatures::PlaylistItem>{ServerError(ServerError::InvalidData, errorMessage)};
        }
    }


}