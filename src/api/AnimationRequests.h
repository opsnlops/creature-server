#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "model/JsonCodec.h"
#include "server/namespace-stuffs.h"
#include "util/UuidValidation.h"

namespace creatures::api {

inline constexpr std::size_t MAX_ANIMATION_CONTROL_REQUEST_BODY_BYTES = 4096;
inline constexpr std::size_t MAX_AD_HOC_SPEECH_REQUEST_BODY_BYTES = 64 * 1024;
inline constexpr std::size_t MAX_AD_HOC_SPEECH_TEXT_BYTES = 32 * 1024;
inline constexpr universe_t MIN_ANIMATION_UNIVERSE = 1;
inline constexpr universe_t MAX_ANIMATION_UNIVERSE = 63999;

struct PlayAnimationRequest {
    std::string animationId;
    universe_t universe;
    bool resumePlaylist{false};
};

struct RegenerateAnimationLipSyncRequest {
    std::string animationId;
};

struct CreateAdHocAnimationRequest {
    std::string creatureId;
    std::string text;
    bool resumePlaylist{true};
};

struct TriggerAdHocAnimationRequest {
    std::string animationId;
    bool resumePlaylist{true};
};

inline Result<std::string> animationRequestUuid(const nlohmann::json &json, std::string_view path,
                                                std::string_view key) {
    auto value = json_codec::requiredString(json, path, key, 36);
    if (!value.isSuccess())
        return value;
    if (!isUuidShape(value.getValue().value()))
        return json_codec::invalid<std::string>(fmt::format("{}.{} must be a UUID", path, key));
    return value;
}

inline Result<PlayAnimationRequest> playAnimationRequestFromJson(const nlohmann::json &json,
                                                                 std::string_view path = "animation play request") {
    auto fields = json_codec::rejectUnknownFields(json, path, {"animation_id", "universe", "resumePlaylist"});
    if (!fields.isSuccess())
        return Result<PlayAnimationRequest>{fields.getError().value()};
    auto animationId = animationRequestUuid(json, path, "animation_id");
    auto universe = json_codec::requiredUnsigned<universe_t>(json, path, "universe", MAX_ANIMATION_UNIVERSE);
    auto resumePlaylist = json_codec::optionalBool(json, path, "resumePlaylist");
    if (!animationId.isSuccess())
        return Result<PlayAnimationRequest>{animationId.getError().value()};
    if (!universe.isSuccess())
        return Result<PlayAnimationRequest>{universe.getError().value()};
    if (universe.getValue().value() < MIN_ANIMATION_UNIVERSE)
        return json_codec::invalid<PlayAnimationRequest>(fmt::format("{}.universe must be at least 1", path));
    if (!resumePlaylist.isSuccess())
        return Result<PlayAnimationRequest>{resumePlaylist.getError().value()};
    return Result<PlayAnimationRequest>{
        {animationId.getValue().value(), universe.getValue().value(), resumePlaylist.getValue()->value_or(false)}};
}

inline Result<RegenerateAnimationLipSyncRequest> regenerateAnimationLipSyncRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "animation lip sync request";
    auto fields = json_codec::rejectUnknownFields(json, path, {"animation_id"});
    if (!fields.isSuccess())
        return Result<RegenerateAnimationLipSyncRequest>{fields.getError().value()};
    auto animationId = animationRequestUuid(json, path, "animation_id");
    if (!animationId.isSuccess())
        return Result<RegenerateAnimationLipSyncRequest>{animationId.getError().value()};
    return Result<RegenerateAnimationLipSyncRequest>{{animationId.getValue().value()}};
}

inline Result<CreateAdHocAnimationRequest> createAdHocAnimationRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "ad-hoc animation request";
    auto fields = json_codec::rejectUnknownFields(json, path, {"creature_id", "text", "resume_playlist"});
    if (!fields.isSuccess())
        return Result<CreateAdHocAnimationRequest>{fields.getError().value()};
    auto creatureId = animationRequestUuid(json, path, "creature_id");
    auto text = json_codec::requiredString(json, path, "text", MAX_AD_HOC_SPEECH_TEXT_BYTES);
    auto resumePlaylist = json_codec::optionalBool(json, path, "resume_playlist");
    if (!creatureId.isSuccess())
        return Result<CreateAdHocAnimationRequest>{creatureId.getError().value()};
    if (!text.isSuccess())
        return Result<CreateAdHocAnimationRequest>{text.getError().value()};
    if (!resumePlaylist.isSuccess())
        return Result<CreateAdHocAnimationRequest>{resumePlaylist.getError().value()};
    return Result<CreateAdHocAnimationRequest>{
        {creatureId.getValue().value(), text.getValue().value(), resumePlaylist.getValue()->value_or(true)}};
}

inline Result<TriggerAdHocAnimationRequest> triggerAdHocAnimationRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "ad-hoc animation trigger request";
    auto fields = json_codec::rejectUnknownFields(json, path, {"animation_id", "resume_playlist"});
    if (!fields.isSuccess())
        return Result<TriggerAdHocAnimationRequest>{fields.getError().value()};
    auto animationId = animationRequestUuid(json, path, "animation_id");
    auto resumePlaylist = json_codec::optionalBool(json, path, "resume_playlist");
    if (!animationId.isSuccess())
        return Result<TriggerAdHocAnimationRequest>{animationId.getError().value()};
    if (!resumePlaylist.isSuccess())
        return Result<TriggerAdHocAnimationRequest>{resumePlaylist.getError().value()};
    return Result<TriggerAdHocAnimationRequest>{
        {animationId.getValue().value(), resumePlaylist.getValue()->value_or(true)}};
}

} // namespace creatures::api
