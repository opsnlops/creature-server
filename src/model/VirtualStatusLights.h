
#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

#include "util/Result.h"

namespace creatures {

/**
 * This represents a "virtual view" of the status lights that would normally be
 * on the server's HAT. This is sent to the client so it can make a "virtual"
 * set of status lights if it wishes.
 */
struct VirtualStatusLights {
    bool running;
    bool dmx;
    bool streaming;
    bool animation_playing;
};

nlohmann::json virtualStatusLightsToJson(const VirtualStatusLights &virtualStatusLights);
Result<VirtualStatusLights> virtualStatusLightsFromJson(const nlohmann::json &json,
                                                         std::string_view path = "virtual_status_lights");

} // namespace creatures
