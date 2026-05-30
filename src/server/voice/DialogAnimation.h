#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "DialogPipeline.h"
#include "model/Animation.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

/// Per-creature inputs to the dialog Animation builder. One entry per creature
/// in the scene; the caller (the dialog worker, in Phase 5) is responsible for:
///
/// 1. Resolving each scene voice_id to its creature record / config.
/// 2. Picking one of the creature's speech_loop_animation_ids and loading +
///    base64-decoding its track frames into `baseFrames`.
/// 3. Rendering the per-frame mouth byte sequence (via TextToViseme to convert
///    `DialogAssembled::perCreature[i].mouth` CharTiming → RhubarbMouthCue,
///    then SoundDataProcessor → uint8_t per frame).
///
/// Phase 4 (this header) is pure: it doesn't touch the database, the viseme
/// dictionary, or the network. It just shapes the inputs into a Track.
struct CreatureTrackInput {
    /// Must match a voiceId in DialogAssembled::perCreature.
    std::string voiceId;

    /// Goes into Track::creature_id verbatim.
    std::string creatureId;

    /// Full stored creature JSON (motors[], inputs[], mouth_slot, ...). The
    /// neutral-frame builder reads motors[] + inputs[] + mouth_slot from here.
    nlohmann::json creatureJson;

    /// Base body-motion frames, already loaded from the DB and base64-decoded.
    /// Index 0..N-1 = one frame each at the scene's msPerFrame; we loop modulo
    /// N when the speaking span runs longer than the base animation. Must be
    /// non-empty; every frame must be the same width.
    std::vector<std::vector<uint8_t>> baseFrames;

    /// Per-frame mouth byte (0 = rest, non-zero = some viseme open shape).
    /// Length must equal the scene's total frame count. Used both as the byte
    /// written into the frame's mouth_slot AND as the "is speaking at frame f"
    /// predicate — any non-zero entry means this creature is being voiced at
    /// that frame and gets base body motion; zero entries → neutral pose.
    std::vector<uint8_t> mouthBytes;
};

/// Build the multi-creature Animation for a finished dialog scene.
///
/// Output: one Animation with N Tracks (one per creature), each Track holding
/// `number_of_frames` base64-encoded frames. Every track has the same frame
/// count (= ceil(total scene ms / msPerFrame)); during a creature's speaking
/// spans its frames carry the looped speech-loop body motion with the mouth
/// byte written at mouth_slot; during silent frames it sits in neutral pose.
/// Metadata carries title, sound_file (path returned by Phase 3's
/// writeDialogWav), milliseconds_per_frame, number_of_frames, and
/// multitrack_audio = true.
///
/// `assembled.perCreature` and `creatureInputs` are matched by voiceId — the
/// order doesn't have to agree. Every voice in `assembled` must have exactly
/// one matching `CreatureTrackInput`; extras in `creatureInputs` are an error
/// (probably indicates a caller bug).
///
/// Speaking-vs-silent is derived from `CreatureTrackInput::mouthBytes`: any
/// frame with a non-zero mouth byte is treated as a speaking frame. (X = rest
/// in the Rhubarb mapping is 0; A/B/C/D/E/F are all non-zero.) A small post-
/// roll keeps the body motion running for a few frames after the last cue,
/// so the body doesn't snap to neutral the instant a turn ends.
Result<Animation> buildDialogAnimation(const DialogAssembled &assembled,
                                       const std::vector<CreatureTrackInput> &creatureInputs, uint32_t msPerFrame,
                                       const std::string &soundFilePath, const std::string &title,
                                       std::shared_ptr<OperationSpan> parentSpan = nullptr);

} // namespace creatures::voice
