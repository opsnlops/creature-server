
#pragma once

#include <vector>
#include <string>
#include <nlohmann/json.hpp>

#include "model/AnimationMetadata.h"
#include "model/FrameData.h"

namespace creatures {

    struct Animation {
        std::string id;
        AnimationMetadata metadata;
        std::vector<FrameData> frames;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Animation, id, metadata, frames)
    };
}
