#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "model/Sound.h"

namespace creatures::api {

struct AdHocSoundEntry {
    std::string animationId;
    std::string createdAt;
    std::string soundFile;
    Sound sound;
};

inline nlohmann::json adHocSoundEntryToJson(const AdHocSoundEntry &entry) {
    return {{"animation_id", entry.animationId},
            {"created_at", entry.createdAt},
            {"sound_file", entry.soundFile},
            {"sound", soundToJson(entry.sound)}};
}

} // namespace creatures::api
