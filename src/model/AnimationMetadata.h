
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "model/DialogScriptTypes.h"
#include "util/Result.h"

namespace creatures {

/// One creature's resolved loop choices for a single render (#119).
struct CreatureRenderChoice {
    std::string creature_id;
    std::string speech_loop_animation_id;
    std::string idle_animation_id; // empty when the creature had none usable
    uint32_t idle_start_offset{0};

    bool operator==(const CreatureRenderChoice &) const = default;
};

struct AnimationMetadata {
    std::string animation_id;
    std::string title;
    uint32_t milliseconds_per_frame;
    std::string note;
    std::string sound_file;
    uint32_t number_of_frames;
    bool multitrack_audio;

    // Soft pointer + copy-on-write snapshot for animations rendered from a
    // saved DialogScript. Both empty for non-dialog animations or for dialog
    // animations submitted with inline turns. `source_script_turns` is a
    // snapshot of the script's turns AT RENDER TIME — preserved so the
    // animation remains traceable even after the parent script is edited
    // or deleted.
    std::string source_script_id;
    std::vector<DialogScriptTurn> source_script_turns;

    // Stage provenance (#119). `source_stage_id` is a soft pointer;
    // `source_stage_updated_at` is the stage's updated_at AT RENDER TIME, so
    // "is this animation stale?" is a comparison rather than a diff.
    std::string source_stage_id;
    int64_t source_stage_updated_at{0};
    // Copy-on-write stage snapshot. Placement extras remain opaque so the
    // neutral contract does not erase console-owned stage data.
    nlohmann::json source_stage_placements = nlohmann::json::array();

    // Seed every random choice in the render derived from — which speech loop
    // and idle animation each creature drew, the idle start phases, and every
    // creature's gaze reaction timing (#119).
    //
    // This is what makes re-rendering against a moved stage trustworthy: with
    // the seed pinned, nudging one creature changes ONLY the gaze bytes.
    // Without it, every creature would reshuffle its idle animation too and
    // you could never see what your edit actually did.
    uint64_t render_seed{0};

    /// Which loops each creature actually drew at render time (#119).
    ///
    /// The seed alone isn't enough to reproduce these: replaying it would
    /// require the re-render to consume the rng in exactly the same order as
    /// the original, and would silently pick differently if a creature gained
    /// an animation in its config since. Recording the resolved choices makes
    /// a re-render reproduce the same body motion by construction, so a stage
    /// edit provably changes only the head aiming.
    std::vector<CreatureRenderChoice> source_render_choices;
};

inline constexpr std::size_t MAX_ANIMATION_TITLE_BYTES = 256;
inline constexpr std::size_t MAX_ANIMATION_NOTE_BYTES = 16384;
inline constexpr std::size_t MAX_ANIMATION_SOUND_FILE_BYTES = 4096;
inline constexpr std::size_t MAX_ANIMATION_RENDER_CHOICES = 16;
inline constexpr std::size_t MAX_ANIMATION_STAGE_PLACEMENTS_BYTES = 64 * 1024;
inline constexpr uint32_t MAX_ANIMATION_MILLISECONDS_PER_FRAME = 1000;

nlohmann::json animationMetadataToJson(const AnimationMetadata &animationMetadata);
Result<AnimationMetadata> animationMetadataFromJson(const nlohmann::json &json,
                                                    std::string_view path = "animation.metadata",
                                                    bool allowTrustedAbsoluteSoundFile = false);
} // namespace creatures
