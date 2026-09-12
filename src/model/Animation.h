
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

/// Where an animation document came from, which decides how strictly it is
/// read. Api: a client upload — relative sound path inside the sound library,
/// no legacy fields. Persistence: a stored document — trusted absolute sound
/// path and the legacy `_id`/`created_at`/`last_updated` fields. AdHoc: a
/// server-rendered ad-hoc animation about to be inserted — its sound_file is
/// the absolute path of its temp WAV (the AdHoc bucket rule), but nothing
/// else about it is legacy, so only the path rule relaxes (issue #188).
enum class AnimationJsonSource { Api, Persistence, AdHoc };

nlohmann::json animationToJson(const Animation &animation);
Result<Animation> animationFromJson(const nlohmann::json &json, AnimationJsonSource source = AnimationJsonSource::Api);

} // namespace creatures
