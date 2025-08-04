
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

    if (!parentSpan) {
        parentSpan = observability->createOperationSpan("get_playlist_from_json_operation");
    }
    auto span = observability->createChildOperationSpan("get_playlist_from_json", parentSpan);

    debug("attempting to create a playlist from JSON via playlistFromJson()");
    debug("JSON size: {} bytes, dump preview: {}", playlistJson.dump().length(),
          playlistJson.dump().substr(0, std::min(200UL, playlistJson.dump().length())));

    // Keep track of what we're working on so we can make a good error message
    std::string working_on;

    try {

        auto playlist = Playlist();
        working_on = "id";
        debug("Validating '{}' field in playlist JSON", working_on);
        if (!playlistJson.contains(working_on) || playlistJson[working_on].is_null()) {
            std::string errorMessage = fmt::format("Missing or null field '{}' in playlist JSON", working_on);
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        playlist.id = playlistJson[working_on];
        debug("Successfully parsed playlist id: '{}'", playlist.id);

        working_on = "name";
        debug("Validating '{}' field in playlist JSON", working_on);
        if (!playlistJson.contains(working_on) || playlistJson[working_on].is_null()) {
            std::string errorMessage = fmt::format("Missing or null field '{}' in playlist JSON", working_on);
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        playlist.name = playlistJson[working_on];
        debug("Successfully parsed playlist name: '{}'", playlist.name);

        working_on = "number_of_items";
        debug("Validating '{}' field in playlist JSON", working_on);
        if (!playlistJson.contains(working_on) || playlistJson[working_on].is_null()) {
            std::string errorMessage = fmt::format("Missing or null field '{}' in playlist JSON", working_on);
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        playlist.number_of_items = playlistJson[working_on];
        debug("Successfully parsed playlist number_of_items: {}", playlist.number_of_items);

        // Add all the items
        working_on = "items";
        debug("Validating '{}' field in playlist JSON (should be array)", working_on);
        if (!playlistJson.contains(working_on) || !playlistJson[working_on].is_array()) {
            std::string errorMessage =
                fmt::format("Missing or invalid field '{}' in playlist JSON (expected array)", working_on);
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        debug("Processing {} playlist items", playlistJson[working_on].size());
        std::vector<json> itemsJson = playlistJson[working_on];
        debug("Starting to process {} playlist items from JSON array", itemsJson.size());
        for (size_t i = 0; i < itemsJson.size(); ++i) {
            debug("Processing playlist item {} of {}", i + 1, itemsJson.size());
            const auto &itemJson = itemsJson[i];
            auto itemResult = playlistItemFromJson(itemJson, span);
            if (!itemResult.isSuccess()) {
                auto error = itemResult.getError();
                warn("Error while creating a playlistItem from JSON while making a playlist: {}", error->getMessage());
                return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, error->getMessage())};
            }
            playlist.items.push_back(itemResult.getValue().value());
            debug("Successfully processed playlist item {} (animation_id: {})", i + 1,
                  itemResult.getValue().value().animation_id);
        }
        debug("Finished processing all {} playlist items", itemsJson.size());

        debug("✅ Successfully created playlist from JSON: id='{}', name='{}', items_count={}", playlist.id,
              playlist.name, playlist.items.size());
        span->setSuccess();
        return Result<creatures::Playlist>{playlist};
    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage =
            fmt::format("Error while creating a playlist from JSON (field '{}'): {} (object: {})", working_on, e.what(),
                        playlistJson.dump(4));
        warn(errorMessage);
        span->setError(errorMessage);
        return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
    }
}

Result<creatures::PlaylistItem> Database::playlistItemFromJson(json playlistItemJson,
                                                               std::shared_ptr<OperationSpan> parentSpan) {

    if (!parentSpan) {
        parentSpan = observability->createOperationSpan("get_playlist_item_from_json_operation");
    }
    auto span = observability->createChildOperationSpan("get_playlist_item_from_json", parentSpan);

    debug("attempting to create a playlistItem from JSON via playlistItemFromJson()");
    debug("PlaylistItem JSON size: {} bytes, preview: {}", playlistItemJson.dump().length(),
          playlistItemJson.dump().substr(0, std::min(100UL, playlistItemJson.dump().length())));

    // Keep track of the element we're working on so we can have good error messages
    std::string working_on;

    try {

        auto playlistItem = PlaylistItem();
        working_on = "animation_id";
        if (!playlistItemJson.contains(working_on) || playlistItemJson[working_on].is_null()) {
            std::string errorMessage = fmt::format("Missing or null field '{}' in playlist item JSON", working_on);
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<creatures::PlaylistItem>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        playlistItem.animation_id = playlistItemJson[working_on];
        debug("animation_id: {}", playlistItem.animation_id);
        span->setAttribute("animation_id", playlistItem.animation_id);

        working_on = "weight";
        if (!playlistItemJson.contains(working_on) || playlistItemJson[working_on].is_null()) {
            std::string errorMessage = fmt::format("Missing or null field '{}' in playlist item JSON", working_on);
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<creatures::PlaylistItem>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        playlistItem.weight = playlistItemJson[working_on];
        debug("weight: {}", playlistItem.weight);
        span->setAttribute("weight", playlistItem.weight);

        debug("done with playlistItemFromJson");

        span->setSuccess();
        return Result<creatures::PlaylistItem>{playlistItem};
    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage =
            fmt::format("Error while creating a playlistItem from JSON (field '{}'): {}", working_on, e.what());
        warn(errorMessage);
        span->recordException(e);
        return Result<creatures::PlaylistItem>{ServerError(ServerError::InvalidData, errorMessage)};
    }
}

} // namespace creatures