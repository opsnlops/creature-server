#include "server/database.h"

#include <spdlog/spdlog.h>

#include "model/Playlist.h"
#include "model/PlaylistItem.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<ObservabilityManager> observability;

Result<Playlist> Database::playlistFromJson(json playlistJson, std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability->createChildOperationSpan("Database.playlistFromJson", parentSpan);
    const auto result = creatures::playlistFromJson(playlistJson);
    if (!result.isSuccess()) {
        const auto error = result.getError().value();
        recordSpanError(span, error.getMessage(), "InvalidData", error.getCode());
        warn("Playlist configuration rejected: {}", error.getMessage());
        return Result<Playlist>{error};
    }

    const auto playlist = result.getValue().value();
    if (span) {
        span->setAttribute("playlist.id", playlist.id);
        span->setAttribute("playlist.name", playlist.name);
        span->setAttribute("playlist.items.count", static_cast<int64_t>(playlist.items.size()));
        span->setSuccess();
    }
    return Result<Playlist>{playlist};
}

Result<PlaylistItem> Database::playlistItemFromJson(json playlistItemJson, std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability->createChildOperationSpan("Database.playlistItemFromJson", parentSpan);
    const auto result = creatures::playlistItemFromJson(playlistItemJson);
    if (!result.isSuccess()) {
        const auto error = result.getError().value();
        recordSpanError(span, error.getMessage(), "InvalidData", error.getCode());
        return Result<PlaylistItem>{error};
    }
    const auto item = result.getValue().value();
    if (span) {
        span->setAttribute("playlist_item.animation_id", item.animation_id);
        span->setAttribute("playlist_item.weight", static_cast<int64_t>(item.weight));
        span->setSuccess();
    }
    return Result<PlaylistItem>{item};
}

Result<Playlist> Database::playlistFromStoredJson(json playlistJson, std::shared_ptr<OperationSpan> parentSpan) {
    if (playlistJson.is_object()) {
        playlistJson.erase("_id");
        for (auto iterator = playlistJson.begin(); iterator != playlistJson.end();) {
            if (iterator.key() != "id" && iterator.key() != "name" && iterator.key() != "items" &&
                iterator.key() != "number_of_items") {
                iterator = playlistJson.erase(iterator);
            } else {
                ++iterator;
            }
        }
        auto items = playlistJson.find("items");
        if (items != playlistJson.end() && items->is_array()) {
            for (auto &item : *items) {
                if (!item.is_object())
                    continue;
                for (auto iterator = item.begin(); iterator != item.end();) {
                    if (iterator.key() != "animation_id" && iterator.key() != "weight") {
                        iterator = item.erase(iterator);
                    } else {
                        ++iterator;
                    }
                }
            }
        }
    }
    return playlistFromJson(std::move(playlistJson), std::move(parentSpan));
}

} // namespace creatures
