#include "TurnConcat.h"

#include <algorithm>
#include <unordered_map>

#include <fmt/format.h>

#include "util/uuidUtils.h"

namespace creatures::voice {

namespace {

/// The frame that mono sample `sample` falls in. Integer maths throughout —
/// frames * (sampleRate * msPerFrame) == samples * 1000 — so there is no
/// floating-point rounding to argue with at part boundaries.
uint64_t frameAtSample(uint64_t sample, uint32_t msPerFrame, uint32_t sampleRate) {
    return (sample * 1000ULL) / (static_cast<uint64_t>(sampleRate) * static_cast<uint64_t>(msPerFrame));
}

} // namespace

Result<ConcatenatedTurnTracks> concatTurnTracks(const std::vector<TurnTrackPart> &parts, uint32_t msPerFrame,
                                                uint32_t sampleRate, const std::string &animationId) {
    if (parts.empty()) {
        return Result<ConcatenatedTurnTracks>{ServerError(ServerError::InvalidData, "concatTurnTracks: no parts")};
    }
    if (msPerFrame == 0 || sampleRate == 0) {
        return Result<ConcatenatedTurnTracks>{
            ServerError(ServerError::InvalidData, "concatTurnTracks: msPerFrame and sampleRate must be > 0")};
    }
    if (parts.front().tracks.empty()) {
        return Result<ConcatenatedTurnTracks>{
            ServerError(ServerError::InvalidData, "concatTurnTracks: first part has no tracks")};
    }

    // Participant order comes from the first part; later parts are matched
    // on creature_id so a caller that happens to order tracks differently
    // still lands each creature's frames on its own track.
    ConcatenatedTurnTracks result;
    result.tracks.reserve(parts.front().tracks.size());
    std::unordered_map<std::string, std::size_t> trackIndexByCreature;
    for (const auto &track : parts.front().tracks) {
        creatures::Track stitched;
        stitched.id = util::generateUUID();
        stitched.creature_id = track.creature_id;
        stitched.animation_id = animationId;
        trackIndexByCreature.emplace(track.creature_id, result.tracks.size());
        result.tracks.push_back(std::move(stitched));
    }

    uint64_t cumulativeSamples = 0;
    for (std::size_t partIndex = 0; partIndex < parts.size(); ++partIndex) {
        const auto &part = parts[partIndex];
        const auto firstFrame = frameAtSample(cumulativeSamples, msPerFrame, sampleRate);
        cumulativeSamples += part.audioSamples;
        const auto endFrame = frameAtSample(cumulativeSamples, msPerFrame, sampleRate);
        const auto frameCount = static_cast<std::size_t>(endFrame - firstFrame);

        if (result.totalFrames + frameCount > MAX_ANIMATION_FRAMES_PER_TRACK) {
            return Result<ConcatenatedTurnTracks>{
                ServerError(ServerError::InvalidData,
                            fmt::format("concatTurnTracks: stitched animation would exceed {} frames per track",
                                        MAX_ANIMATION_FRAMES_PER_TRACK))};
        }

        for (auto &stitched : result.tracks) {
            const auto source = std::find_if(part.tracks.begin(), part.tracks.end(), [&](const creatures::Track &t) {
                return t.creature_id == stitched.creature_id;
            });
            if (source == part.tracks.end()) {
                return Result<ConcatenatedTurnTracks>{ServerError(
                    ServerError::InvalidData, fmt::format("concatTurnTracks: part {} has no track for creature '{}'",
                                                          partIndex + 1, stitched.creature_id))};
            }
            if (source->frames.empty() && frameCount > 0) {
                return Result<ConcatenatedTurnTracks>{ServerError(
                    ServerError::InvalidData, fmt::format("concatTurnTracks: part {} track for creature '{}' is empty",
                                                          partIndex + 1, stitched.creature_id))};
            }
            // Trim to the audio-derived count, or repeat the last frame to
            // fill — the body holds still rather than jumping ahead.
            const auto available = source->frames.size();
            for (std::size_t f = 0; f < frameCount; ++f) {
                stitched.frames.push_back(source->frames[std::min(f, available - 1)]);
            }
        }
        result.totalFrames += frameCount;
    }

    return result;
}

} // namespace creatures::voice
