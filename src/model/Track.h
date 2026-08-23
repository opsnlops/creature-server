
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/Result.h"

namespace creatures {

struct Track {
    std::string id;
    std::string creature_id; // Set for creature tracks; empty for fixture tracks
    std::string fixture_id;  // Set for fixture tracks; empty for creature tracks
    std::string animation_id;
    std::vector<std::string> frames; // The frame data will be base64 encoded strings
};

inline constexpr std::size_t MAX_ANIMATION_FRAMES_PER_TRACK = 250000;
inline constexpr std::size_t MAX_ANIMATION_FRAME_DECODED_BYTES = 512;
inline constexpr std::size_t MAX_ANIMATION_FRAME_ENCODED_BYTES = 684;

/// Framework-neutral wire/persistence representation. Populated targets are
/// emitted and absent targets are omitted; input validation owns the XOR rule.
nlohmann::json trackToJson(const Track &track);
Result<Track> trackFromJson(const nlohmann::json &json, std::string_view path = "track",
                            bool allowLegacyNullOptionals = false);

} // namespace creatures
