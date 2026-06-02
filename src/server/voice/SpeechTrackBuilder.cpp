#include "SpeechTrackBuilder.h"

#include <algorithm>
#include <limits>

#include <base64.hpp>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "util/uuidUtils.h"

namespace creatures::voice {

namespace {

// Decode a Track's base64 frames into raw byte vectors. Returns InvalidData
// if any frame fails to decode.
Result<std::vector<std::vector<uint8_t>>> decodeFrames(const std::vector<std::string> &encodedFrames,
                                                       const std::string &creatureId) {
    std::vector<std::vector<uint8_t>> out;
    out.reserve(encodedFrames.size());
    for (std::size_t i = 0; i < encodedFrames.size(); ++i) {
        try {
            const std::string raw = base64::from_base64(encodedFrames[i]);
            out.emplace_back(raw.begin(), raw.end());
        } catch (const std::exception &e) {
            return Result<std::vector<std::vector<uint8_t>>>{ServerError(
                ServerError::InvalidData,
                fmt::format("Creature '{}' base-frame[{}] base64 decode failed: {}", creatureId, i, e.what()))};
        }
    }
    return out;
}

} // namespace

Result<ResolvedSpeechBase> resolveSpeechBaseFrames(const creatures::Creature &creature, creatures::Database &db,
                                                   std::mt19937 &rng, std::shared_ptr<OperationSpan> parentSpan) {
    if (creature.speech_loop_animation_ids.empty()) {
        return Result<ResolvedSpeechBase>{
            ServerError(ServerError::InvalidData,
                        fmt::format("Creature '{}' has no speech_loop_animation_ids configured", creature.name))};
    }

    std::uniform_int_distribution<std::size_t> dist(0, creature.speech_loop_animation_ids.size() - 1);
    const auto chosenId = creature.speech_loop_animation_ids[dist(rng)];

    auto animResult = db.getAnimation(chosenId, parentSpan);
    if (!animResult.isSuccess()) {
        return Result<ResolvedSpeechBase>{ServerError(
            ServerError::DatabaseError, fmt::format("Unable to load speech loop animation '{}' for creature '{}': {}",
                                                    chosenId, creature.name, animResult.getError()->getMessage()))};
    }
    const auto baseAnim = animResult.getValue().value();

    // Find the track for this creature. Helpful failure message tells the
    // author exactly what to fix in their content.
    auto trackIt = std::find_if(baseAnim.tracks.begin(), baseAnim.tracks.end(),
                                [&](const Track &t) { return t.creature_id == creature.id; });
    if (trackIt == baseAnim.tracks.end()) {
        return Result<ResolvedSpeechBase>{ServerError(
            ServerError::InvalidData,
            fmt::format("Speech loop animation '{}' does not have a track for creature '{}'. Add a track for '{}' to "
                        "this animation, or remove it from the creature's speech_loop_animation_ids list.",
                        baseAnim.metadata.title, creature.name, creature.name))};
    }
    if (trackIt->frames.empty()) {
        return Result<ResolvedSpeechBase>{ServerError(
            ServerError::InvalidData, fmt::format("Speech loop track for '{}' in animation '{}' has no frames",
                                                  creature.name, baseAnim.metadata.title))};
    }

    auto decodeResult = decodeFrames(trackIt->frames, creature.id);
    if (!decodeResult.isSuccess()) {
        return Result<ResolvedSpeechBase>{decodeResult.getError().value()};
    }
    auto baseFrames = decodeResult.getValue().value();

    // Width consistency check. All frames must be the same width or the
    // modular cycle in buildSpeechTrack would shift mouth_slot mid-scene.
    const std::size_t frameWidth = baseFrames.front().size();
    if (frameWidth == 0) {
        return Result<ResolvedSpeechBase>{ServerError(
            ServerError::InvalidData, fmt::format("Creature '{}' speech-loop baseFrames[0] is empty", creature.name))};
    }
    for (std::size_t f = 1; f < baseFrames.size(); ++f) {
        if (baseFrames[f].size() != frameWidth) {
            return Result<ResolvedSpeechBase>{
                ServerError(ServerError::InvalidData,
                            fmt::format("Creature '{}' speech-loop baseFrames[{}] width {} != baseFrames[0] width {}",
                                        creature.name, f, baseFrames[f].size(), frameWidth))};
        }
    }

    ResolvedSpeechBase resolved;
    resolved.baseFrames = std::move(baseFrames);
    resolved.baseAnimationId = baseAnim.id;
    resolved.baseAnimationTitle = baseAnim.metadata.title;
    resolved.baseMsPerFrame = baseAnim.metadata.milliseconds_per_frame;
    resolved.baseAnimation = std::move(baseAnim);

    if (parentSpan) {
        parentSpan->setAttribute("speech_loop.animation_id", baseAnim.id);
        parentSpan->setAttribute("speech_loop.frame_count", static_cast<int64_t>(resolved.baseFrames.size()));
    }
    return resolved;
}

Result<SpeechTrackResult> buildSpeechTrack(const SpeechTrackInput &input, const SpeechTrackOptions &options,
                                           std::shared_ptr<OperationSpan> parentSpan) {
    if (input.baseFrames.empty()) {
        return Result<SpeechTrackResult>{
            ServerError(ServerError::InvalidData,
                        fmt::format("buildSpeechTrack: creature '{}' has no baseFrames", input.creatureId))};
    }
    const std::size_t frameWidth = input.baseFrames.front().size();
    if (frameWidth == 0) {
        return Result<SpeechTrackResult>{
            ServerError(ServerError::InvalidData,
                        fmt::format("buildSpeechTrack: creature '{}' baseFrames[0] is empty", input.creatureId))};
    }

    // Single mouth_slot bounds check — the canonical defensive Beaky-chest
    // guard (issue #15 / 3.14.4). Out-of-range → silently drop mouth bytes
    // for the whole track; one warn at most.
    const bool mouthSlotInRange = input.mouthSlot < frameWidth;
    if (!mouthSlotInRange) {
        warn("buildSpeechTrack: creature '{}' mouth_slot {} >= frame width {} — mouth bytes will not be written for "
             "this track",
             input.creatureId, input.mouthSlot, frameWidth);
        if (parentSpan) {
            parentSpan->setAttribute("mouth.slot_out_of_range", true);
        }
    }

    // Build the speakingAt mask. In simple mode every frame is "speaking"
    // (advance the body counter). In dialogIdleMode the mask is derived
    // from mouthBytes: non-zero = active, extended forward by bodyTailFrames
    // so the body doesn't snap to neutral the instant a turn ends.
    std::vector<bool> speakingAt(input.totalFrames, true);
    if (options.dialogIdleMode) {
        std::fill(speakingAt.begin(), speakingAt.end(), false);
        std::size_t lastActive = std::numeric_limits<std::size_t>::max();
        for (std::size_t f = 0; f < input.totalFrames; ++f) {
            const bool active = (f < input.mouthBytes.size()) && (input.mouthBytes[f] != 0);
            if (active) {
                lastActive = f;
                speakingAt[f] = true;
            } else if (lastActive != std::numeric_limits<std::size_t>::max() &&
                       f - lastActive <= options.bodyTailFrames) {
                speakingAt[f] = true;
            }
        }
    }

    // Emit frames. Speaking frames cycle baseFrames (continuing from
    // startOffset, advancing per speaking frame). Silent frames (only
    // possible in dialogIdleMode) freeze on baseFrames[0] — the speech
    // loop's authoritative idle pose (3.15.3 design decision).
    SpeechTrackResult result;
    result.mouthSlotInRange = mouthSlotInRange;
    result.track.id = util::generateUUID();
    result.track.creature_id = input.creatureId;
    result.track.animation_id = input.animationId;
    result.track.frames.reserve(input.totalFrames);

    std::size_t speakingCounter = options.startOffset;
    for (std::size_t f = 0; f < input.totalFrames; ++f) {
        std::vector<uint8_t> frame;
        if (speakingAt[f]) {
            frame = input.baseFrames[speakingCounter % input.baseFrames.size()];
            if (mouthSlotInRange && f < input.mouthBytes.size()) {
                frame[input.mouthSlot] = input.mouthBytes[f];
            }
            ++speakingCounter;
            ++result.speakingFrameCount;
        } else {
            // dialogIdleMode silent frame — freeze on baseFrames[0].
            frame = input.baseFrames.front();
        }
        std::string raw(reinterpret_cast<const char *>(frame.data()), frame.size());
        result.track.frames.push_back(base64::to_base64(raw));
    }

    // For streaming continuity: where would the body counter land at the
    // start of the next sentence? speakingCounter has been advanced once per
    // speaking frame, modulo bookkeeping. Convert to a 0-based offset into
    // baseFrames for the next caller.
    result.endOffset = speakingCounter % input.baseFrames.size();

    if (parentSpan) {
        parentSpan->setAttribute("track.creature_id", input.creatureId);
        parentSpan->setAttribute("track.total_frames", static_cast<int64_t>(input.totalFrames));
        parentSpan->setAttribute("track.speaking_frames", static_cast<int64_t>(result.speakingFrameCount));
    }
    return result;
}

} // namespace creatures::voice
