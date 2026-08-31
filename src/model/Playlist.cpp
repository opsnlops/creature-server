
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "model/JsonCodec.h"
#include "model/Playlist.h"
#include "model/PlaylistItem.h"
#include "util/UuidValidation.h"
#include "util/helpers.h"

namespace creatures {

namespace {

template <typename T> Result<Playlist> forwardPlaylistError(const Result<T> &result) {
    return Result<Playlist>{result.getError().value()};
}

} // namespace

nlohmann::json playlistToJson(const Playlist &playlist) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto &item : playlist.items)
        items.push_back(playlistItemToJson(item));
    return {{"id", isUuidShape(playlist.id) ? canonicalUuid(playlist.id) : playlist.id},
            {"name", playlist.name},
            {"items", std::move(items)},
            {"number_of_items", static_cast<uint32_t>(playlist.items.size())}};
}

Result<Playlist> playlistFromJson(const nlohmann::json &json, std::string_view path) {
    try {
        auto fields = json_codec::rejectUnknownFields(json, path, {"id", "name", "items", "number_of_items"});
        if (!fields.isSuccess())
            return forwardPlaylistError(fields);

        auto id = json_codec::requiredString(json, path, "id", 36);
        auto name = json_codec::requiredString(json, path, "name", MAX_PLAYLIST_NAME_BYTES);
        auto numberOfItems = json_codec::requiredUnsigned<uint32_t>(json, path, "number_of_items", MAX_PLAYLIST_ITEMS);
        if (!id.isSuccess())
            return forwardPlaylistError(id);
        if (!name.isSuccess())
            return forwardPlaylistError(name);
        if (!numberOfItems.isSuccess())
            return forwardPlaylistError(numberOfItems);
        if (!isUuidShape(id.getValue().value()))
            return json_codec::invalid<Playlist>(fmt::format("{}.id must be a UUID", path));

        auto itemsResult = json_codec::requiredArray(json, path, "items", MAX_PLAYLIST_ITEMS, 1);
        if (!itemsResult.isSuccess())
            return forwardPlaylistError(itemsResult);
        const auto &items = itemsResult.getValue()->get();
        if (numberOfItems.getValue().value() != items.size()) {
            return json_codec::invalid<Playlist>(fmt::format("{}.number_of_items must equal items.size()", path));
        }

        Playlist playlist;
        playlist.id = canonicalUuid(id.getValue().value());
        playlist.name = name.getValue().value();
        playlist.number_of_items = numberOfItems.getValue().value();
        playlist.items.reserve(items.size());
        std::unordered_set<animationId_t> animationIds;
        for (std::size_t index = 0; index < items.size(); ++index) {
            auto item = playlistItemFromJson(items[index], fmt::format("{}.items[{}]", path, index));
            if (!item.isSuccess())
                return forwardPlaylistError(item);
            if (!animationIds.emplace(item.getValue()->animation_id).second) {
                return json_codec::invalid<Playlist>(
                    fmt::format("{}.items[{}].animation_id must not duplicate an earlier item", path, index));
            }
            playlist.items.push_back(item.getValue().value());
        }
        auto weight = playlistTotalWeight(playlist);
        if (!weight.isSuccess())
            return forwardPlaylistError(weight);
        return Result<Playlist>{playlist};
    } catch (const nlohmann::json::exception &error) {
        return json_codec::invalid<Playlist>(fmt::format("{} is invalid JSON: {}", path, error.what()));
    }
}

Result<uint64_t> playlistTotalWeight(const Playlist &playlist) {
    uint64_t totalWeight = 0;
    for (const auto &item : playlist.items) {
        if (item.weight > std::numeric_limits<uint64_t>::max() - totalWeight) {
            return Result<uint64_t>{
                ServerError(ServerError::InvalidData, "Playlist total weight exceeds the supported range")};
        }
        totalWeight += item.weight;
    }
    return Result<uint64_t>{totalWeight};
}

Result<animationId_t> playlistAnimationAtWeight(const Playlist &playlist, uint64_t selectedWeight) {
    uint64_t cumulativeWeight = 0;
    for (const auto &item : playlist.items) {
        if (item.weight > std::numeric_limits<uint64_t>::max() - cumulativeWeight) {
            return Result<animationId_t>{
                ServerError(ServerError::InvalidData, "Playlist total weight exceeds the supported range")};
        }
        cumulativeWeight += item.weight;
        if (selectedWeight < cumulativeWeight) {
            return Result<animationId_t>{item.animation_id};
        }
    }
    return Result<animationId_t>{
        ServerError(ServerError::InvalidData, "Selected playlist weight is outside the playlist total")};
}

} // namespace creatures
