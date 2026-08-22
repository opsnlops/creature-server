
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "model/AnimationMetadata.h"
#include "model/Track.h"
#include "util/Result.h"

namespace creatures {

struct Animation {
    std::string id;
    AnimationMetadata metadata;
    std::vector<Track> tracks;
};

inline constexpr std::size_t MAX_ANIMATION_TRACKS = 64;
// Bounds per-frame allocation and decode work independently of serialized byte size.
inline constexpr std::size_t MAX_ANIMATION_TOTAL_FRAME_ENTRIES = 500000;
inline constexpr std::size_t MAX_ANIMATION_TOTAL_ENCODED_FRAME_BYTES = 12ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t MAX_ANIMATION_REQUEST_BODY_BYTES = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t MAX_ANIMATION_PERSISTED_BYTES = 15ULL * 1024ULL * 1024ULL;
inline constexpr uint64_t MAX_ANIMATION_DURATION_MS = 24ULL * 60ULL * 60ULL * 1000ULL;

enum class AnimationJsonSource { Api, Persistence };

nlohmann::json animationToJson(const Animation &animation);
Result<Animation> animationFromJson(const nlohmann::json &json, AnimationJsonSource source = AnimationJsonSource::Api);

} // namespace creatures
