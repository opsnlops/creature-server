#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "model/Creature.h"
#include "model/Track.h"
#include "server/database.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

// Shared helpers for "build a speech track for one creature" — the core work
// that StreamingAdHocSession, processAdHocSpeechJob, and DialogAnimation all
// did inline before issue #15. The Beaky-chest crash fixed in 3.14.4 only
// happened because the three paths had drifted on the mouth_slot bounds
// check; consolidating them here closes that class of bug.
namespace creatures::voice {

// =============================================================================
// resolveSpeechBaseFrames — pick a speech_loop_animation_ids entry, look up
// the animation, find the right track, decode base64 frames, validate widths.
// =============================================================================

struct ResolvedSpeechBase {
    // Decoded base-track frames. Every frame has the same width (validated
    // here so the per-frame loop in buildSpeechTrack doesn't have to).
    std::vector<std::vector<uint8_t>> baseFrames;

    // The full base animation — callers use this as a template for the
    // rendered Animation (StreamingAdHoc + processAdHocSpeechJob both copy
    // from it then override id / title / sound_file / etc.).
    creatures::Animation baseAnimation;

    // Convenience shortcuts pulled from baseAnimation. Duplicated here so
    // callers that don't need the full Animation don't have to reach into it.
    std::string baseAnimationId;
    std::string baseAnimationTitle;
    uint32_t baseMsPerFrame{0};
};

// Loads + resolves the base frames a speech track will cycle. RNG comes in
// by reference so tests can pin the choice with a seeded generator;
// production callers thread their existing rng through unchanged.
//
// Returns InvalidData on: empty speech_loop_animation_ids, base animation
// missing the creature's track, empty frames, inconsistent frame widths.
// DatabaseError if the DB lookup itself fails.
Result<ResolvedSpeechBase> resolveSpeechBaseFrames(const creatures::Creature &creature, creatures::Database &db,
                                                   std::mt19937 &rng,
                                                   std::shared_ptr<OperationSpan> parentSpan = nullptr);

// =============================================================================
// buildSpeechTrack — assemble the per-creature Track from decoded base frames
// + a per-frame mouth-byte stream. Handles all three call sites' variants via
// an options struct.
// =============================================================================

struct SpeechTrackInput {
    // Already-decoded base frames. All frames must be the same width
    // (resolveSpeechBaseFrames guarantees this; dialog callers validate
    // separately when assembling their creatureInputs).
    std::span<const std::vector<uint8_t>> baseFrames;

    // Per-target-frame mouth byte. 0 = mouth at rest. Bytes 5..255 = active
    // viseme (any non-zero value is treated as "speaking" for idle-mode
    // purposes). May be shorter than totalFrames — trailing frames just write
    // a 0 (no mouth byte).
    std::span<const uint8_t> mouthBytes;

    // Byte index in each frame where the mouth value lives. Bounds-checked
    // against frame width — if out of range, the mouth byte is silently
    // dropped and SpeechTrackResult.mouthSlotInRange is false (and a single
    // warn is logged). This is the canonical Beaky-chest defensive check.
    std::size_t mouthSlot{0};

    // How many output frames to produce. Base frames are cycled to fill.
    std::size_t totalFrames{0};

    // Stamped onto the resulting Track.
    std::string creatureId;
    std::string animationId;
};

struct SpeechTrackOptions {
    // Starting offset into baseFrames. StreamingAdHoc uses the prior
    // sentence's endOffset for continuity across sentence boundaries;
    // ad-hoc speech + dialog start at 0.
    std::size_t startOffset{0};

    // Dialog-only. When true, frames where the *extended* mouth signal is 0
    // freeze on baseFrames[0] (the speech-loop's idle pose) instead of
    // advancing the body counter. "Extended" = the raw mouthBytes mask with
    // each speaking run extended forward by bodyTailFrames so the body
    // doesn't snap to neutral the instant a turn ends.
    bool dialogIdleMode{false};

    // Frames to extend each speaking run forward when dialogIdleMode is on.
    // 0 disables the tail (silent immediately after the last non-zero mouth
    // byte). Ignored when dialogIdleMode is false.
    std::size_t bodyTailFrames{0};
};

struct SpeechTrackResult {
    creatures::Track track;

    // For span attributes + debug logs. speakingFrameCount tracks how many
    // output frames advanced the body counter (in dialogIdleMode) or all
    // frames (in simple mode).
    std::size_t speakingFrameCount{0};

    // The (startOffset + totalFrames) % baseFrames.size() value. Streaming
    // callers feed this into the next sentence's startOffset for continuity.
    // AdHoc + dialog ignore it.
    std::size_t endOffset{0};

    // False → mouthSlot was out of range for the frame width; mouth bytes
    // were silently dropped. Caller may want to surface this on a span.
    bool mouthSlotInRange{true};
};

// The shared inner loop. Cycles base frames + (when in range) writes mouth
// byte at mouthSlot. Optionally separates speaking vs idle frames per the
// dialog rules.
Result<SpeechTrackResult> buildSpeechTrack(const SpeechTrackInput &input, const SpeechTrackOptions &options = {},
                                           std::shared_ptr<OperationSpan> parentSpan = nullptr);

} // namespace creatures::voice
