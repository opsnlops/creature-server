
#pragma once

#include <vector>
#include <string>
#include <nlohmann/json.hpp>

namespace creatures {

    struct FrameData {
        std::string id;
        std::string creature_id;
        std::string animation_id;
        std::vector<std::string> frames;  // The frame data will be base64 encoded strings

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(FrameData, id, creature_id, animation_id, frames)
    };
}