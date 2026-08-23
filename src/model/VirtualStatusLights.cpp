

#include "model/VirtualStatusLights.h"
#include "model/JsonCodec.h"

namespace creatures {

namespace {

Result<bool> requiredBoolean(const nlohmann::json &json, std::string_view path, std::string_view key) {
    const auto iterator = json.find(key);
    if (iterator == json.end())
        return json_codec::invalid<bool>(fmt::format("{}.{} is required", path, key));
    if (!iterator->is_boolean())
        return json_codec::invalid<bool>(fmt::format("{}.{} must be a boolean", path, key));
    return Result<bool>{iterator->get<bool>()};
}

} // namespace

nlohmann::json virtualStatusLightsToJson(const VirtualStatusLights &lights) {
    return {{"running", lights.running},
            {"dmx", lights.dmx},
            {"streaming", lights.streaming},
            {"animation_playing", lights.animation_playing}};
}

Result<VirtualStatusLights> virtualStatusLightsFromJson(const nlohmann::json &json, std::string_view path) {
    auto fields = json_codec::rejectUnknownFields(json, path, {"running", "dmx", "streaming", "animation_playing"});
    if (!fields.isSuccess())
        return Result<VirtualStatusLights>{fields.getError().value()};
    auto running = requiredBoolean(json, path, "running");
    auto dmx = requiredBoolean(json, path, "dmx");
    auto streaming = requiredBoolean(json, path, "streaming");
    auto animationPlaying = requiredBoolean(json, path, "animation_playing");
    if (!running.isSuccess())
        return Result<VirtualStatusLights>{running.getError().value()};
    if (!dmx.isSuccess())
        return Result<VirtualStatusLights>{dmx.getError().value()};
    if (!streaming.isSuccess())
        return Result<VirtualStatusLights>{streaming.getError().value()};
    if (!animationPlaying.isSuccess())
        return Result<VirtualStatusLights>{animationPlaying.getError().value()};

    return Result<VirtualStatusLights>{VirtualStatusLights{running.getValue().value(), dmx.getValue().value(),
                                                           streaming.getValue().value(),
                                                           animationPlaying.getValue().value()}};
}

} // namespace creatures
