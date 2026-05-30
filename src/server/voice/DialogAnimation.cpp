#include "DialogAnimation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <base64.hpp>
#include <fmt/format.h>

#include "server/namespace-stuffs.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::voice {

namespace {

/// How many extra frames the body keeps moving after a creature stops voicing,
/// before snapping back to the idle pose. Keeps the body from looking jerky at
/// end-of-turn. At a typical 20ms/frame this is ~100ms of release tail.
constexpr std::size_t kBodyTailFrames = 5;

/// Match `assembled.perCreature` (indexed by voiceId) to `creatureInputs`
/// (also indexed by voiceId). Returns the parallel index from creatureInputs
/// for each perCreature entry, or fails if any voice is unmatched.
Result<std::vector<std::size_t>> matchVoices(const DialogAssembled &assembled,
                                             const std::vector<CreatureTrackInput> &creatureInputs) {
    std::unordered_map<std::string, std::size_t> byVoice;
    byVoice.reserve(creatureInputs.size());
    for (std::size_t i = 0; i < creatureInputs.size(); ++i) {
        const auto &ci = creatureInputs[i];
        if (!byVoice.emplace(ci.voiceId, i).second) {
            return Result<std::vector<std::size_t>>{
                ServerError(ServerError::InvalidData,
                            fmt::format("buildDialogAnimation: duplicate voiceId '{}' in creatureInputs", ci.voiceId))};
        }
    }

    std::vector<std::size_t> matched;
    matched.reserve(assembled.perCreature.size());
    for (const auto &pc : assembled.perCreature) {
        auto it = byVoice.find(pc.voiceId);
        if (it == byVoice.end()) {
            return Result<std::vector<std::size_t>>{ServerError(
                ServerError::InvalidData,
                fmt::format("buildDialogAnimation: voiceId '{}' is in the scene but has no CreatureTrackInput",
                            pc.voiceId))};
        }
        matched.push_back(it->second);
    }
    return matched;
}

} // namespace

Result<Animation> buildDialogAnimation(const DialogAssembled &assembled,
                                       const std::vector<CreatureTrackInput> &creatureInputs, uint32_t msPerFrame,
                                       const std::string &soundFilePath, const std::string &title,
                                       std::shared_ptr<OperationSpan> parentSpan,
                                       const std::string &existingAnimationId) {
    auto span = creatures::observability->createChildOperationSpan("DialogAnimation.buildDialogAnimation", parentSpan);
    if (span) {
        span->setAttribute("animation.title", title);
        span->setAttribute("animation.sound_file", soundFilePath);
        span->setAttribute("animation.ms_per_frame", static_cast<int64_t>(msPerFrame));
        span->setAttribute("animation.creatures", static_cast<int64_t>(assembled.perCreature.size()));
        span->setAttribute("animation.reuse_id", !existingAnimationId.empty());
    }

    if (msPerFrame == 0) {
        std::string msg = "buildDialogAnimation: msPerFrame must be > 0";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<Animation>{ServerError(ServerError::InvalidData, msg)};
    }
    if (assembled.totalSamples == 0 || assembled.perCreature.empty()) {
        std::string msg = "buildDialogAnimation: assembled scene is empty";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<Animation>{ServerError(ServerError::InvalidData, msg)};
    }
    if (assembled.sampleRate == 0) {
        std::string msg = "buildDialogAnimation: assembled.sampleRate must be > 0";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<Animation>{ServerError(ServerError::InvalidData, msg)};
    }
    if (creatureInputs.size() != assembled.perCreature.size()) {
        std::string msg = fmt::format("buildDialogAnimation: {} creatureInputs, {} scene voices (must match 1:1)",
                                      creatureInputs.size(), assembled.perCreature.size());
        error(msg);
        if (span)
            span->setError(msg);
        return Result<Animation>{ServerError(ServerError::InvalidData, msg)};
    }

    auto matchResult = matchVoices(assembled, creatureInputs);
    if (!matchResult.isSuccess()) {
        std::string msg = matchResult.getError()->getMessage();
        error(msg);
        if (span)
            span->setError(msg);
        return Result<Animation>{matchResult.getError().value()};
    }
    const auto matched = matchResult.getValue().value();

    // Scene length in animation frames. Round up so the last partial frame
    // gets emitted; mouth byte buffers may end one frame earlier than the
    // sample-aligned scene length, which is fine — they're zero-padded
    // implicitly via vector bounds-checking below.
    const double totalMs =
        static_cast<double>(assembled.totalSamples) * 1000.0 / static_cast<double>(assembled.sampleRate);
    const auto totalFrames = static_cast<std::size_t>(std::ceil(totalMs / static_cast<double>(msPerFrame)));
    if (totalFrames == 0) {
        std::string msg = "buildDialogAnimation: scene rounds to zero frames";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<Animation>{ServerError(ServerError::InvalidData, msg)};
    }

    Animation animation;
    // Re-render path: keep the existing animation_id so the DB upsert overwrites
    // the previous render in place (script.id is the script, animation.id is
    // the rendered artifact; one script → one animation, the latest one wins).
    // Fresh render: brand-new UUID.
    animation.id = existingAnimationId.empty() ? util::generateUUID() : existingAnimationId;
    animation.metadata.animation_id = animation.id;
    animation.metadata.title = title;
    animation.metadata.milliseconds_per_frame = msPerFrame;
    animation.metadata.number_of_frames = static_cast<uint32_t>(totalFrames);
    animation.metadata.sound_file = soundFilePath;
    animation.metadata.multitrack_audio = true; // 17-channel WAV per Phase 3.
    animation.metadata.note = fmt::format("Generated dialog scene ({} creatures)", assembled.perCreature.size());
    animation.tracks.reserve(assembled.perCreature.size());

    for (std::size_t pcIdx = 0; pcIdx < assembled.perCreature.size(); ++pcIdx) {
        const auto &input = creatureInputs[matched[pcIdx]];

        // Frame width comes from the base animation — every base frame must
        // be the same width or the modular loop below would shift mouth_slot
        // mid-scene.
        if (input.baseFrames.empty()) {
            std::string msg = fmt::format("buildDialogAnimation: creature '{}' has no baseFrames", input.creatureId);
            error(msg);
            if (span)
                span->setError(msg);
            return Result<Animation>{ServerError(ServerError::InvalidData, msg)};
        }
        const std::size_t frameWidth = input.baseFrames.front().size();
        if (frameWidth == 0) {
            std::string msg =
                fmt::format("buildDialogAnimation: creature '{}' baseFrames[0] is empty", input.creatureId);
            error(msg);
            if (span)
                span->setError(msg);
            return Result<Animation>{ServerError(ServerError::InvalidData, msg)};
        }
        for (std::size_t f = 1; f < input.baseFrames.size(); ++f) {
            if (input.baseFrames[f].size() != frameWidth) {
                std::string msg =
                    fmt::format("buildDialogAnimation: creature '{}' baseFrames[{}] width {} != baseFrames[0] width {}",
                                input.creatureId, f, input.baseFrames[f].size(), frameWidth);
                error(msg);
                if (span)
                    span->setError(msg);
                return Result<Animation>{ServerError(ServerError::InvalidData, msg)};
            }
        }

        // mouth_slot from the creature config — bounds-check against the base
        // frame width once here (rather than per-frame), and skip the write if
        // out of range. Mirrors the ad-hoc path's defensive behavior at
        // StreamingAdHocSession.cpp:344-355.
        std::size_t mouthSlot = std::numeric_limits<std::size_t>::max();
        if (input.creatureJson.contains("mouth_slot") && input.creatureJson["mouth_slot"].is_number()) {
            mouthSlot = input.creatureJson["mouth_slot"].get<std::size_t>();
        }
        const bool mouthSlotInRange = mouthSlot < frameWidth;
        if (!mouthSlotInRange) {
            warn("buildDialogAnimation: creature '{}' mouth_slot {} >= frame width {} — mouth byte will not be written",
                 input.creatureId, mouthSlot, frameWidth);
        }

        // Idle pose for this creature when she's NOT speaking — use the first
        // frame of her own speech_loop animation. Earlier we tried to compute
        // a neutral frame from the creature's motor config (default_position +
        // inverted), but that re-derived "what is idle?" from a config the
        // author already encoded into the speech_loop itself, and got the
        // inverted-motor convention subtly wrong (3.15.3 fix: Beaky's
        // body_lean stuck high = leaning forward, when the speech_loop's idle
        // pose has it low = upright). The speech_loop's frame 0 is the
        // authoritative idle pose by construction.
        const auto &idleFrame = input.baseFrames.front();

        // Derive speakingAt[] from mouthBytes: any non-zero byte = a viseme
        // shape (A/B/C/D/E/F = 5..255), zero = rest (X). Then extend each
        // speaking run forward by kBodyTailFrames so the body doesn't snap
        // to neutral the instant a turn ends. Pre-roll is implicit — the
        // first mouth byte of a turn already lands at the start of the body
        // motion, which is the natural onset.
        std::vector<bool> speakingAt(totalFrames, false);
        std::size_t lastActive = std::numeric_limits<std::size_t>::max();
        for (std::size_t f = 0; f < totalFrames; ++f) {
            const bool active = (f < input.mouthBytes.size()) && (input.mouthBytes[f] != 0);
            if (active) {
                lastActive = f;
                speakingAt[f] = true;
            } else if (lastActive != std::numeric_limits<std::size_t>::max() && f - lastActive <= kBodyTailFrames) {
                speakingAt[f] = true;
            }
        }

        // Build the Track. Speaking frames use the base body motion advanced
        // by a per-creature counter so motion stays continuous across the
        // creature's own turns (body picks up where it left off, not where
        // the global frame index points). Silent frames freeze on the
        // speech_loop's first frame (idleFrame) so the listening creature
        // holds her correct idle pose without inventing one from motor config.
        Track track;
        track.id = util::generateUUID();
        track.creature_id = input.creatureId;
        track.animation_id = animation.id;
        track.frames.reserve(totalFrames);

        std::size_t speakingCounter = 0;
        std::size_t speakingFrameCount = 0;
        for (std::size_t f = 0; f < totalFrames; ++f) {
            std::vector<uint8_t> frame;
            if (speakingAt[f]) {
                frame = input.baseFrames[speakingCounter % input.baseFrames.size()];
                if (mouthSlotInRange && f < input.mouthBytes.size()) {
                    frame[mouthSlot] = input.mouthBytes[f];
                }
                ++speakingCounter;
                ++speakingFrameCount;
            } else {
                frame = idleFrame;
            }
            std::string raw(reinterpret_cast<const char *>(frame.data()), frame.size());
            track.frames.push_back(base64::to_base64(raw));
        }
        debug("buildDialogAnimation: creature '{}' (voice '{}'): {} total frames, {} speaking, {} silent",
              input.creatureId, input.voiceId, totalFrames, speakingFrameCount, totalFrames - speakingFrameCount);

        animation.tracks.push_back(std::move(track));
    }

    info("buildDialogAnimation: built animation '{}' for {} creatures, {} frames ({:.2f}s) @ {}ms/frame", animation.id,
         animation.tracks.size(), totalFrames, totalMs / 1000.0, msPerFrame);

    if (span) {
        span->setAttribute("animation.id", animation.id);
        span->setAttribute("animation.frames", static_cast<int64_t>(totalFrames));
        span->setAttribute("animation.duration_s", totalMs / 1000.0);
        span->setSuccess();
    }
    return animation;
}

} // namespace creatures::voice
