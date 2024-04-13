
#pragma once

#include <vector>
#include <string>
#include <nlohmann/json.hpp>

namespace creatures {

    struct AnimationMetadata {
        std::string animation_id;
        std::string title;
        uint32_t milliseconds_per_frame;
        std::string note;
        std::string sound_file;
        uint32_t number_of_frames;
        bool multitrack_audio;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(AnimationMetadata, animation_id, title, milliseconds_per_frame, note, sound_file, number_of_frames, multitrack_audio)
    };
}
