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

// Which per-creature animation list a resolve call is drawing from. Only
// affects error-message wording and span attributes — the resolve/decode/
// validate work is identical for both.
enum class LoopKind {
    Speech, // Creature::speech_loop_animation_ids
    Idle,   // Creature::idle_animation_ids
};

// The shared resolver: pick one id at random from `animationIds`, load it,
// find the creature's track, decode the base64 frames, validate widths. RNG
// comes in by reference so tests can pin the choice with a seeded generator;
// production callers thread their existing rng through unchanged.
//
// Returns InvalidData on: empty animationIds, animation missing the creature's
// track, empty frames, inconsistent frame widths. DatabaseError if the DB
// lookup itself fails.
Result<ResolvedSpeechBase> resolveLoopFrames(const creatures::Creature &creature,
                                             const std::vector<std::string> &animationIds, LoopKind kind,
                                             creatures::Database &db, std::mt19937 &rng,
                                             std::shared_ptr<OperationSpan> parentSpan = nullptr);

// Loads + resolves the base frames a speech track will cycle.
Result<ResolvedSpeechBase> resolveSpeechBaseFrames(const creatures::Creature &creature, creatures::Database &db,
                                                   std::mt19937 &rng,
                                                   std::shared_ptr<OperationSpan> parentSpan = nullptr);

// Same, for the idle loop a listening creature cycles during silence (#119).
// Callers treat failure as non-fatal: the track falls back to freezing on
// baseFrames[0], which is exactly the pre-#119 behavior.
Result<ResolvedSpeechBase> resolveIdleBaseFrames(const creatures::Creature &creature, creatures::Database &db,
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

    // ---- Head-look layer (#119) -----------------------------------------
    // Per-frame servo bytes for the creature's head axes, produced by
    // GazeTrack. Written LAST, over whatever the body loop put in those
    // slots — the loops carry absolute positions, so adding an offset would
    // clip. Empty (the default) leaves both slots entirely alone, which is
    // what a render with no stage bound produces.
    std::span<const uint8_t> gazePanBytes{};
    std::span<const uint8_t> gazeElevationBytes{};
    std::span<const uint8_t> gazeCockBytes{};
    std::size_t gazePanSlot{0};
    std::size_t gazeElevationSlot{0};
    std::size_t gazeCockSlot{0};
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

    // ---- Idle loop during silence (#119) --------------------------------
    // Frames of the creature's chosen idle animation, cycled during silent
    // stretches instead of freezing on baseFrames[0]. Empty (the default)
    // reproduces the pre-#119 freeze exactly, byte for byte — every fallback
    // in the resolver funnels here, so a missing or malformed idle animation
    // degrades to old behavior rather than failing the render.
    //
    // Must be the same frame width as baseFrames; the caller validates that
    // and passes an empty span if it doesn't hold.
    std::span<const std::vector<uint8_t>> idleFrames{};

    // Starting phase into idleFrames. Randomized per creature so that two
    // creatures who happen to draw the SAME idle animation don't loop in
    // lockstep — which reads as broken rather than as idle.
    std::size_t idleStartOffset{0};

    // Byte-wise linear blend applied across each speech<->idle transition, so
    // the body eases between the two loops instead of snapping. 0 disables.
    // Ignored when idleFrames is empty.
    std::size_t crossfadeFrames{10};

    // Value forced at mouthSlot on silent frames, but ONLY when idleFrames is
    // in use. An idle animation is authored to play standalone and may move
    // the beak; under someone else's voice that reads as silent mouthing. The
    // frozen-baseFrames[0] fallback deliberately does NOT get this treatment —
    // that frame is the authoritative rest pose, mouth value included.
    std::uint8_t mouthRestByte{0};
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

    // True → silent frames cycled an idle loop; false → they froze on
    // baseFrames[0] (either because no idle animation resolved, or because
    // this isn't dialogIdleMode). For span attributes and debug logs.
    bool idleFramesUsed{false};

    // True → gaze bytes were actually written for that axis. False can mean
    // "no stage bound", "creature has no such axis", or "slot out of range".
    bool gazePanApplied{false};
    bool gazeElevationApplied{false};
    // Cock is the exception: true means the axis was eligible, but it is only
    // written on frames where the creature is listening — while it speaks, its
    // body loop keeps the tilt (issue #144).
    bool gazeCockApplied{false};
};

// The shared inner loop. Cycles base frames + (when in range) writes mouth
// byte at mouthSlot. Optionally separates speaking vs idle frames per the
// dialog rules.
Result<SpeechTrackResult> buildSpeechTrack(const SpeechTrackInput &input, const SpeechTrackOptions &options = {},
                                           std::shared_ptr<OperationSpan> parentSpan = nullptr);

} // namespace creatures::voice
