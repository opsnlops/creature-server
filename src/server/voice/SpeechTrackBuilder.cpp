#include "SpeechTrackBuilder.h"

#include <algorithm>
#include <cmath>
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

// Human-readable name for a loop kind, used in error messages so the content
// author knows which list to go fix.
const char *loopLabel(LoopKind kind) { return kind == LoopKind::Speech ? "speech loop" : "idle loop"; }
const char *loopConfigField(LoopKind kind) {
    return kind == LoopKind::Speech ? "speech_loop_animation_ids" : "idle_animation_ids";
}

} // namespace

Result<ResolvedSpeechBase> resolveLoopFrames(const creatures::Creature &creature,
                                             const std::vector<std::string> &animationIds, LoopKind kind,
                                             creatures::Database &db, std::mt19937 &rng,
                                             std::shared_ptr<OperationSpan> parentSpan) {
    if (animationIds.empty()) {
        return Result<ResolvedSpeechBase>{
            ServerError(ServerError::InvalidData,
                        fmt::format("Creature '{}' has no {} configured", creature.name, loopConfigField(kind)))};
    }

    std::uniform_int_distribution<std::size_t> dist(0, animationIds.size() - 1);
    const auto chosenId = animationIds[dist(rng)];

    auto animResult = db.getAnimation(chosenId, parentSpan);
    if (!animResult.isSuccess()) {
        return Result<ResolvedSpeechBase>{
            ServerError(ServerError::DatabaseError,
                        fmt::format("Unable to load {} animation '{}' for creature '{}': {}", loopLabel(kind), chosenId,
                                    creature.name, animResult.getError()->getMessage()))};
    }
    const auto baseAnim = animResult.getValue().value();

    // Find the track for this creature. Helpful failure message tells the
    // author exactly what to fix in their content.
    auto trackIt = std::find_if(baseAnim.tracks.begin(), baseAnim.tracks.end(),
                                [&](const Track &t) { return t.creature_id == creature.id; });
    if (trackIt == baseAnim.tracks.end()) {
        return Result<ResolvedSpeechBase>{ServerError(
            ServerError::InvalidData,
            fmt::format("{} animation '{}' does not have a track for creature '{}'. Add a track for '{}' to "
                        "this animation, or remove it from the creature's {} list.",
                        loopLabel(kind), baseAnim.metadata.title, creature.name, creature.name,
                        loopConfigField(kind)))};
    }
    if (trackIt->frames.empty()) {
        return Result<ResolvedSpeechBase>{ServerError(
            ServerError::InvalidData, fmt::format("{} track for '{}' in animation '{}' has no frames", loopLabel(kind),
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
        return Result<ResolvedSpeechBase>{
            ServerError(ServerError::InvalidData,
                        fmt::format("Creature '{}' {} baseFrames[0] is empty", creature.name, loopLabel(kind)))};
    }
    for (std::size_t f = 1; f < baseFrames.size(); ++f) {
        if (baseFrames[f].size() != frameWidth) {
            return Result<ResolvedSpeechBase>{
                ServerError(ServerError::InvalidData,
                            fmt::format("Creature '{}' {} baseFrames[{}] width {} != baseFrames[0] width {}",
                                        creature.name, loopLabel(kind), f, baseFrames[f].size(), frameWidth))};
        }
    }

    ResolvedSpeechBase resolved;
    resolved.baseFrames = std::move(baseFrames);
    resolved.baseAnimationId = baseAnim.id;
    resolved.baseAnimationTitle = baseAnim.metadata.title;
    resolved.baseMsPerFrame = baseAnim.metadata.milliseconds_per_frame;
    resolved.baseAnimation = std::move(baseAnim);

    if (parentSpan) {
        const std::string prefix = kind == LoopKind::Speech ? "speech_loop" : "idle_loop";
        parentSpan->setAttribute(prefix + ".animation_id", resolved.baseAnimationId);
        parentSpan->setAttribute(prefix + ".frame_count", static_cast<int64_t>(resolved.baseFrames.size()));
    }
    return resolved;
}

Result<ResolvedSpeechBase> resolveSpeechBaseFrames(const creatures::Creature &creature, creatures::Database &db,
                                                   std::mt19937 &rng, std::shared_ptr<OperationSpan> parentSpan) {
    return resolveLoopFrames(creature, creature.speech_loop_animation_ids, LoopKind::Speech, db, rng,
                             std::move(parentSpan));
}

Result<ResolvedSpeechBase> resolveIdleBaseFrames(const creatures::Creature &creature, creatures::Database &db,
                                                 std::mt19937 &rng, std::shared_ptr<OperationSpan> parentSpan) {
    return resolveLoopFrames(creature, creature.idle_animation_ids, LoopKind::Idle, db, rng, std::move(parentSpan));
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

    // Head-look slots get the same treatment as mouth_slot: one bounds check
    // up front, and out-of-range means "drop this axis" rather than "fail the
    // render" or "write past the end of the frame".
    const bool panSlotInRange = !input.gazePanBytes.empty() && input.gazePanSlot < frameWidth;
    const bool elevationSlotInRange = !input.gazeElevationBytes.empty() && input.gazeElevationSlot < frameWidth;
    const bool cockSlotInRange = !input.gazeCockBytes.empty() && input.gazeCockSlot < frameWidth;
    if (!input.gazePanBytes.empty() && !panSlotInRange) {
        warn("buildSpeechTrack: creature '{}' gaze pan slot {} >= frame width {} — pan will not be written",
             input.creatureId, input.gazePanSlot, frameWidth);
    }
    if (!input.gazeElevationBytes.empty() && !elevationSlotInRange) {
        warn("buildSpeechTrack: creature '{}' gaze elevation slot {} >= frame width {} — elevation will not be written",
             input.creatureId, input.gazeElevationSlot, frameWidth);
    }
    if (!input.gazeCockBytes.empty() && !cockSlotInRange) {
        warn("buildSpeechTrack: creature '{}' gaze cock slot {} >= frame width {} — cock will not be written",
             input.creatureId, input.gazeCockSlot, frameWidth);
    }

    // Decide up front whether we can actually cycle an idle loop during
    // silence (#119). Any doubt → fall back to the pre-#119 freeze, which is
    // byte-for-byte what this function used to emit. A creature with no idle
    // animation, or one authored at a different width, must never fail a
    // render just because it can't idle prettily.
    const bool useIdleLoop =
        options.dialogIdleMode && !options.idleFrames.empty() && options.idleFrames.front().size() == frameWidth;
    if (options.dialogIdleMode && !options.idleFrames.empty() && !useIdleLoop) {
        warn("buildSpeechTrack: creature '{}' idle frames are {} bytes wide but the speech loop is {} — freezing on "
             "baseFrames[0] instead of cycling the idle loop",
             input.creatureId, options.idleFrames.front().size(), frameWidth);
    }

    // Emit frames. Speaking frames cycle baseFrames (continuing from
    // startOffset, advancing per speaking frame). Silent frames (only
    // possible in dialogIdleMode) either cycle the idle loop from its own
    // free-running counter, or — when no idle loop is available — freeze on
    // baseFrames[0], the speech loop's authoritative idle pose (3.15.3).
    SpeechTrackResult result;
    result.mouthSlotInRange = mouthSlotInRange;
    result.idleFramesUsed = useIdleLoop;
    result.gazePanApplied = panSlotInRange;
    result.gazeElevationApplied = elevationSlotInRange;
    result.gazeCockApplied = cockSlotInRange;
    result.track.id = util::generateUUID();
    result.track.creature_id = input.creatureId;
    result.track.animation_id = input.animationId;
    result.track.frames.reserve(input.totalFrames);

    // Crossfade state. `fadeSource` is a snapshot of the last body frame
    // emitted before a speech<->idle transition; we ease from it toward the
    // new loop's (moving) frame so the body doesn't snap between loops.
    std::vector<uint8_t> fadeSource;
    std::vector<uint8_t> lastBody;
    std::size_t fadeRemaining = 0;
    const std::size_t fadeTotal = useIdleLoop ? options.crossfadeFrames : 0;

    // Cock-axis ownership (issue #144). The cock aims at nothing — it's the
    // quizzical listening tilt — and a speaking creature holds it level, so
    // the gaze layer emits a dead constant for the whole of every turn it
    // holds. On a differential neck (head_height + head_tilt -> neck_left /
    // neck_right) that strips most of the life out of the head: Beaky's tilt
    // went from 82 distinct values in her loop to literally one in the render.
    //
    // So the loop owns head_tilt while a creature is speaking, and gaze owns it
    // while it's listening. Ownership changes are eased over the same window as
    // the body crossfade, because a hard handover between two unrelated values
    // is exactly the twitch this axis is supposed to avoid.
    //
    // Note this is a WRITE-side decision only — buildGazeTrack still computes
    // the full cock stream and consumes its rng identically, so `render_seed`
    // reproducibility is unaffected.
    const std::size_t cockFadeTotal = options.crossfadeFrames;
    int cockFadeSource = -1;
    int lastCockByte = -1;
    std::size_t cockFadeRemaining = 0;
    bool cockOwnedByGazePrev = false;

    std::size_t speakingCounter = options.startOffset;
    std::size_t idleCounter = options.idleStartOffset;
    for (std::size_t f = 0; f < input.totalFrames; ++f) {
        // A mode change arms the crossfade, easing out of whatever body pose
        // we were just holding.
        if (f > 0 && fadeTotal > 0 && speakingAt[f] != speakingAt[f - 1]) {
            fadeSource = lastBody;
            fadeRemaining = fadeTotal;
        }

        std::vector<uint8_t> frame;
        if (speakingAt[f]) {
            frame = input.baseFrames[speakingCounter % input.baseFrames.size()];
            ++speakingCounter;
            ++result.speakingFrameCount;
        } else if (useIdleLoop) {
            frame = options.idleFrames[idleCounter % options.idleFrames.size()];
            ++idleCounter;
        } else {
            frame = input.baseFrames.front();
        }

        // Blend the body BEFORE the mouth byte goes in, so the mouth is never
        // smeared across a transition.
        if (fadeRemaining > 0 && fadeSource.size() == frame.size()) {
            const double t = static_cast<double>(fadeTotal - fadeRemaining + 1) / static_cast<double>(fadeTotal + 1);
            for (std::size_t i = 0; i < frame.size(); ++i) {
                const double from = static_cast<double>(fadeSource[i]);
                const double to = static_cast<double>(frame[i]);
                frame[i] = static_cast<uint8_t>(std::lround(from + (to - from) * t));
            }
            --fadeRemaining;
        }
        lastBody = frame;

        if (mouthSlotInRange) {
            if (speakingAt[f]) {
                if (f < input.mouthBytes.size()) {
                    frame[input.mouthSlot] = input.mouthBytes[f];
                }
            } else if (useIdleLoop) {
                // An idle animation may move the beak; under another
                // creature's voice that reads as silent mouthing. Pin it shut.
                frame[input.mouthSlot] = options.mouthRestByte;
            }
            // else: frozen baseFrames[0] keeps its own mouth value, exactly
            // as it did pre-#119.
        }

        // Head-look is the last layer: it overwrites whatever the body loop
        // put in the head slots. Each axis is bounds-checked independently, so
        // a creature with a good pan slot and a bad tilt slot still pans.
        if (panSlotInRange && f < input.gazePanBytes.size()) {
            frame[input.gazePanSlot] = input.gazePanBytes[f];
        }
        if (elevationSlotInRange && f < input.gazeElevationBytes.size()) {
            frame[input.gazeElevationSlot] = input.gazeElevationBytes[f];
        }
        if (cockSlotInRange && f < input.gazeCockBytes.size()) {
            // Listening -> gaze aims the tilt. Speaking -> whatever the body
            // loop already put in this slot stays.
            const bool cockOwnedByGaze = !speakingAt[f];
            if (f > 0 && cockOwnedByGaze != cockOwnedByGazePrev && cockFadeTotal > 0 && lastCockByte >= 0) {
                cockFadeSource = lastCockByte;
                cockFadeRemaining = cockFadeTotal;
            }
            cockOwnedByGazePrev = cockOwnedByGaze;

            int cockByte = cockOwnedByGaze ? static_cast<int>(input.gazeCockBytes[f])
                                           : static_cast<int>(frame[input.gazeCockSlot]);
            if (cockFadeRemaining > 0 && cockFadeSource >= 0) {
                // Ease from the value we last emitted toward the new owner's
                // (moving) value, mirroring how the body crossfade works.
                const double t =
                    static_cast<double>(cockFadeTotal - cockFadeRemaining + 1) / static_cast<double>(cockFadeTotal + 1);
                cockByte = static_cast<int>(
                    std::lround(static_cast<double>(cockFadeSource) +
                                (static_cast<double>(cockByte) - static_cast<double>(cockFadeSource)) * t));
                --cockFadeRemaining;
            }
            frame[input.gazeCockSlot] = static_cast<uint8_t>(std::clamp(cockByte, 0, 255));
            lastCockByte = cockByte;
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
        parentSpan->setAttribute("track.idle_frames_used", result.idleFramesUsed);
        parentSpan->setAttribute("track.gaze_pan_applied", result.gazePanApplied);
        parentSpan->setAttribute("track.gaze_elevation_applied", result.gazeElevationApplied);
        parentSpan->setAttribute("track.gaze_cock_applied", result.gazeCockApplied);
    }
    return result;
}

} // namespace creatures::voice
