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
#include "server/voice/SpeechTrackBuilder.h"
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
constexpr std::size_t kMaxDialogAnimationFrames = 120000;
constexpr std::uint64_t kMaxEstimatedAnimationBsonBytes = 12ULL * 1024 * 1024;

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
                                       const std::string &existingAnimationId, std::size_t minimumTotalSamples) {
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
    const auto showSamples = std::max(assembled.totalSamples, minimumTotalSamples);
    if (span) {
        span->setAttribute("animation.dialog_samples", static_cast<int64_t>(assembled.totalSamples));
        span->setAttribute("animation.show_samples", static_cast<int64_t>(showSamples));
    }
    const double totalMs = static_cast<double>(showSamples) * 1000.0 / static_cast<double>(assembled.sampleRate);
    const auto totalFrames = static_cast<std::size_t>(std::ceil(totalMs / static_cast<double>(msPerFrame)));
    if (span) {
        span->setAttribute("animation.frames", static_cast<int64_t>(totalFrames));
    }
    if (totalFrames == 0) {
        std::string msg = "buildDialogAnimation: scene rounds to zero frames";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<Animation>{ServerError(ServerError::InvalidData, msg)};
    }
    if (totalFrames > kMaxDialogAnimationFrames || totalFrames > std::numeric_limits<uint32_t>::max()) {
        std::string msg = fmt::format("buildDialogAnimation: {} frames exceeds the {}-frame safety limit", totalFrames,
                                      kMaxDialogAnimationFrames);
        error(msg);
        if (span)
            recordSpanError(span, msg, "AnimationFrameLimitExceeded", ServerError::InvalidData);
        return Result<Animation>{ServerError(ServerError::InvalidData, msg)};
    }

    // MongoDB documents cap at 16 MiB. Conservatively estimate each base64
    // frame as a BSON array string element before materializing any tracks.
    // Besides the string itself, every element carries a type byte, an array
    // index cstring (up to 10 digits for uint32 frame indices), a four-byte
    // string length, and its terminating NUL. Keep 4 MiB of additional
    // headroom for track documents, metadata, ids, and script provenance.
    std::uint64_t estimatedBsonBytes = 64 * 1024;
    for (const auto &input : creatureInputs) {
        if (input.baseFrames.empty()) {
            continue; // The canonical validation below returns the precise error.
        }
        const auto frameWidth = static_cast<std::uint64_t>(input.baseFrames.front().size());
        const auto encodedFrameBytes = 4ULL * ((frameWidth + 2) / 3);
        constexpr std::uint64_t kBsonArrayElementOverhead = 1 + 10 + 1 + 4 + 1;
        const auto bytesPerFrame = encodedFrameBytes + kBsonArrayElementOverhead;
        if (bytesPerFrame > 0 &&
            static_cast<std::uint64_t>(totalFrames) >
                (kMaxEstimatedAnimationBsonBytes - std::min(estimatedBsonBytes, kMaxEstimatedAnimationBsonBytes)) /
                    bytesPerFrame) {
            estimatedBsonBytes = kMaxEstimatedAnimationBsonBytes + 1;
            break;
        }
        estimatedBsonBytes += static_cast<std::uint64_t>(totalFrames) * bytesPerFrame;
    }
    if (span) {
        span->setAttribute("animation.estimated_bson_bytes", static_cast<int64_t>(estimatedBsonBytes));
    }
    if (estimatedBsonBytes > kMaxEstimatedAnimationBsonBytes) {
        std::string msg = fmt::format("buildDialogAnimation: estimated animation document size exceeds {} bytes",
                                      kMaxEstimatedAnimationBsonBytes);
        error(msg);
        if (span)
            recordSpanError(span, msg, "AnimationDocumentSizeLimitExceeded", ServerError::InvalidData);
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

        // Resolve mouth_slot from the creature JSON. The shared builder
        // bounds-checks against the frame width and skips the write if out
        // of range — the canonical Beaky-chest defensive guard (3.14.4 fix).
        // Prefer the degree-of-freedom reference (#120): if the config names
        // the input driving the mouth, resolve its slot from inputs[]. The
        // raw mouth_slot number is the fallback for configs that predate it.
        std::size_t mouthSlot = std::numeric_limits<std::size_t>::max();
        if (input.creatureJson.contains("mouth_input") && input.creatureJson["mouth_input"].is_string() &&
            input.creatureJson.contains("inputs") && input.creatureJson["inputs"].is_array()) {
            const auto wanted = input.creatureJson["mouth_input"].get<std::string>();
            for (const auto &inputJson : input.creatureJson["inputs"]) {
                if (inputJson.is_object() && inputJson.value("name", std::string{}) == wanted &&
                    inputJson.contains("slot") && inputJson["slot"].is_number()) {
                    mouthSlot = inputJson["slot"].get<std::size_t>();
                    break;
                }
            }
        }
        if (mouthSlot == std::numeric_limits<std::size_t>::max() && input.creatureJson.contains("mouth_slot") &&
            input.creatureJson["mouth_slot"].is_number()) {
            mouthSlot = input.creatureJson["mouth_slot"].get<std::size_t>();
        }

        // Frame-width consistency check stays here — it's input validation on
        // the per-creature data passed to us by the caller, not part of the
        // per-frame assembly the builder owns.
        if (!input.baseFrames.empty()) {
            const std::size_t frameWidth = input.baseFrames.front().size();
            for (std::size_t f = 1; f < input.baseFrames.size(); ++f) {
                if (input.baseFrames[f].size() != frameWidth) {
                    std::string msg = fmt::format(
                        "buildDialogAnimation: creature '{}' baseFrames[{}] width {} != baseFrames[0] width {}",
                        input.creatureId, f, input.baseFrames[f].size(), frameWidth);
                    error(msg);
                    if (span)
                        span->setError(msg);
                    return Result<Animation>{ServerError(ServerError::InvalidData, msg)};
                }
            }
        }

        // Build the per-creature track via the shared builder (issue #15).
        // dialogIdleMode + kBodyTailFrames carry the dialog-specific behavior:
        // each speaking run extends forward by kBodyTailFrames so the body
        // doesn't snap the instant a turn ends, and silent frames cycle the
        // creature's idle loop (#119) — or, when it has none, freeze on
        // baseFrames[0] exactly as they did before (3.15.3).
        SpeechTrackInput trackInput;
        trackInput.baseFrames = input.baseFrames;
        trackInput.mouthBytes = input.mouthBytes;
        trackInput.mouthSlot = mouthSlot;
        trackInput.totalFrames = totalFrames;
        trackInput.creatureId = input.creatureId;
        trackInput.animationId = animation.id;
        trackInput.gazePanBytes = input.gazePanBytes;
        trackInput.gazeElevationBytes = input.gazeElevationBytes;
        trackInput.gazeCockBytes = input.gazeCockBytes;
        trackInput.gazePanSlot = input.gazePanSlot;
        trackInput.gazeElevationSlot = input.gazeElevationSlot;
        trackInput.gazeCockSlot = input.gazeCockSlot;
        SpeechTrackOptions trackOptions;
        trackOptions.dialogIdleMode = true;
        trackOptions.bodyTailFrames = kBodyTailFrames;
        trackOptions.idleFrames = input.idleFrames;
        trackOptions.idleStartOffset = input.idleStartOffset;
        auto trackResult = buildSpeechTrack(trackInput, trackOptions, span);
        if (!trackResult.isSuccess()) {
            return Result<Animation>{trackResult.getError().value()};
        }
        debug("buildDialogAnimation: creature '{}' (voice '{}'): {} total frames, {} speaking, {} silent ({})",
              input.creatureId, input.voiceId, totalFrames, trackResult.getValue()->speakingFrameCount,
              totalFrames - trackResult.getValue()->speakingFrameCount,
              trackResult.getValue()->idleFramesUsed ? "cycling idle loop" : "frozen on baseFrames[0]");

        animation.tracks.push_back(std::move(trackResult.getValue()->track));
    }

    info("buildDialogAnimation: built animation '{}' for {} creatures, {} frames ({:.2f}s) @ {}ms/frame", animation.id,
         animation.tracks.size(), totalFrames, totalMs / 1000.0, msPerFrame);

    if (span) {
        span->setAttribute("animation.id", animation.id);
        span->setAttribute("animation.duration_s", totalMs / 1000.0);
        span->setSuccess();
    }
    return animation;
}

} // namespace creatures::voice
