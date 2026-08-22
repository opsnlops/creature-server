

#include "model/PlaylistItem.h"

#include "model/JsonCodec.h"
#include "util/helpers.h"

namespace creatures {

namespace {

template <typename T> Result<PlaylistItem> forwardError(const Result<T> &result) {
    return Result<PlaylistItem>{result.getError().value()};
}

} // namespace

nlohmann::json playlistItemToJson(const PlaylistItem &playlistItem) {
    return {{"animation_id", playlistItem.animation_id}, {"weight", playlistItem.weight}};
}

Result<PlaylistItem> playlistItemFromJson(const nlohmann::json &json, std::string_view path) {
    auto fieldsResult = json_codec::rejectUnknownFields(json, path, {"animation_id", "weight"});
    if (!fieldsResult.isSuccess())
        return forwardError(fieldsResult);

    auto animationIdResult = json_codec::requiredString(json, path, "animation_id", 36);
    if (!animationIdResult.isSuccess())
        return forwardError(animationIdResult);
    if (!isUuidShape(animationIdResult.getValue().value())) {
        return json_codec::invalid<PlaylistItem>(fmt::format("{}.animation_id must be a UUID", path));
    }

    auto weightResult = json_codec::requiredUnsigned<uint32_t>(json, path, "weight", MAX_PLAYLIST_ITEM_WEIGHT);
    if (!weightResult.isSuccess())
        return forwardError(weightResult);
    if (weightResult.getValue().value() == 0) {
        return json_codec::invalid<PlaylistItem>(fmt::format("{}.weight must be greater than zero", path));
    }

    return Result<PlaylistItem>{PlaylistItem{animationIdResult.getValue().value(), weightResult.getValue().value()}};
}

} // namespace creatures
