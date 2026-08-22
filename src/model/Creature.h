
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "Input.h"

#include "server/namespace-stuffs.h"
#include "util/Result.h"

/**
 * Note to myself for later!
 *
 * Don't get confused by `id` and `_id` in Mongo. `id` is what we use. It can be
 * basically any string. Normally it looks like an OID, but it doesn't have to.
 * It could be a UUID or whatever.
 */

namespace creatures {

/**
 * Angular calibration for one gaze axis (issue #119).
 *
 * `input` names an entry in the creature's `inputs` list — "neck_rotate",
 * "head_height", "head_tilt". The slot is resolved from that name at render
 * time rather than repeated here, because the two creature families lay their
 * inputs out differently (Beaky/Crow put neck_rotate at slot 2; Mango/Kenny
 * put it at 5) and a hand-copied slot number would be a second place to get
 * that wrong.
 *
 * The degrees are the genuinely new information: nothing in the controller
 * config says how far a neck actually sweeps. Servos are described by pulse
 * width, and those vary enormously between birds (Beaky's neck_rotate is
 * 1400-1900us; Kenny's is 650-2500us), so the angular range has to be
 * measured and stated per creature.
 *
 * These are angles in INPUT space, not servo space — the controller owns the
 * servo-level `inverted` flag and, for the differential neck, the conversion
 * from head_height + head_tilt into neck_left/neck_right. Swapping min and max
 * expresses a reversed axis without needing a separate flag.
 */
struct GazeAxis {
    std::string input;          // name of the entry in `inputs`
    float degrees_at_min{0.0f}; // angle when the input byte is 0
    float degrees_at_max{0.0f}; // angle when the input byte is 255

    // Only read for the `cock` axis: how much of the available range a
    // listening creature may tilt its head by. 0 disables the head-cock.
    float listening_amount{0.4f};

    bool operator==(const GazeAxis &) const = default;
};

/**
 * Which of a creature's inputs the gaze layer may drive, and how far.
 *
 * The axis names describe intent, not hardware:
 *
 *   pan       - turns the head left/right. Normally `neck_rotate`.
 *   elevation - raises/lowers the head to aim at a taller or shorter
 *               creature. Normally `head_height`. NOT `head_tilt` — on the
 *               differential neck, head_height moves both neck servos
 *               together while head_tilt drives them apart.
 *   cock      - the quizzical sideways head tilt. Normally `head_tilt`.
 *               Purely expressive: it doesn't aim at anything, it just makes
 *               a listening creature look like it's paying attention.
 *
 * Every axis is optional and independent. A creature with none is rendered
 * exactly as it was before the gaze layer existed.
 */
struct GazeConfig {
    std::optional<GazeAxis> pan;
    std::optional<GazeAxis> elevation;
    std::optional<GazeAxis> cock;

    bool operator==(const GazeConfig &) const = default;
};

struct Creature {

    /**
     * The ID of the creature
     */
    creatureId_t id;

    /**
     * The name of the creature
     */
    std::string name;

    /**
     * The offset of the channel for this creature in the universe
     */
    uint16_t channel_offset;

    /**
     * The audio channel for this creature
     */
    uint16_t audio_channel;

    /**
     * The slot in the motion array that corresponds to the creature's mouth.
     * This is used by the Rhubarb Lip Sync system to automatically generate
     * mouth movements during animation.
     *
     * NOTE (issue #120): this is a raw slot NUMBER, which is the older style.
     * The invariant it has to satisfy is that it points at the creature's beak
     * degree of freedom — the slot of the input named "beak". The number
     * itself is meaningless and differs per creature, because the two creature
     * families lay their inputs out differently. Prefer `mouth_input` for new
     * configs; use `resolvedMouthSlot()` rather than reading this directly.
     */
    uint8_t mouth_slot;

    /**
     * Name of the input that drives the mouth — normally "beak" (issue #120).
     *
     * This is the DEGREE-OF-FREEDOM style, and it's the direction the config
     * is moving: a semantic reference names the axis it means and the slot is
     * resolved per creature, so the same config text works on a bird whose
     * beak is at slot 5 and one whose beak is at slot 2. When set, it wins
     * over `mouth_slot`. Empty means fall back to the raw number.
     */
    std::string mouth_input;

    /**
     * The inputs for this creature
     */
    std::vector<Input> inputs;

    /**
     * A list of animation IDs that can be used as base speech loops for
     * dynamically generated dialogue.
     */
    std::vector<std::string> speech_loop_animation_ids;

    /**
     * A list of animation IDs that can be used as idle loops for this creature.
     */
    std::vector<std::string> idle_animation_ids;

    /**
     * Which inputs the gaze layer may drive, and their angular ranges
     * (issue #119). Optional: absent means this creature never turns to look
     * at anything, and its frames are unchanged.
     *
     * This describes hardware, so it lives on the creature config rather than
     * on the stage — a bird's neck sweep doesn't change when you move its
     * perch.
     */
    std::optional<GazeConfig> gaze;

    /**
     * Runtime state is managed in memory and not persisted with the config.
     * This struct can be extended later to hold runtime values when needed.
     */
    struct RuntimeState {};
};

inline constexpr std::size_t MAX_CREATURE_REQUEST_BODY_BYTES = 1024ULL * 1024ULL;
inline constexpr std::size_t MAX_CREATURE_NAME_BYTES = 128;
inline constexpr std::size_t MAX_CREATURE_INPUTS = 64;
inline constexpr std::size_t MAX_CREATURE_ANIMATION_IDS_PER_LIST = 256;
inline constexpr std::size_t MAX_CREATURE_INPUT_SLOT_END = 512;

/// Canonical framework-neutral configuration representation. Runtime state is
/// intentionally absent: the controller configuration is the source of truth,
/// while runtime state belongs to the server process.
nlohmann::json creatureToJson(const Creature &creature);

/// Parse and validate the modeled portion of a controller-owned config.
/// Unknown top-level fields are deliberately tolerated so newer controller
/// firmware can preserve hardware-specific settings that this server does not
/// yet model. Nested modeled objects are strict.
Result<Creature> creatureFromJson(const nlohmann::json &json, std::string_view path = "creature");

/**
 * Slot of the input with the given name, or nullopt if this creature has no
 * such input.
 *
 * The single place the name-to-slot lookup lives. Every semantic reference to
 * a degree of freedom — the mouth, each gaze axis — goes through here rather
 * than hard-coding a number, because slot layouts differ per creature:
 * Beaky/Crow put "beak" at slot 5, Mango/Kenny put it at 2.
 */
[[nodiscard]] std::optional<uint16_t> inputSlotByName(const Creature &creature, const std::string &inputName);

/**
 * The slot lip-sync should write into: `mouth_input` resolved by name if it's
 * set and resolvable, otherwise the raw `mouth_slot`.
 */
[[nodiscard]] uint16_t resolvedMouthSlot(const Creature &creature);

/**
 * Does `mouth_slot` actually point at this creature's beak?
 *
 * Returns nullopt when the creature has no "beak" input (nothing to check
 * against), true/false otherwise. Callers surface a false as a config warning
 * — a mismatch means the viseme stream is driving whatever DOF happens to sit
 * at that slot instead of the beak. See issue #120.
 */
[[nodiscard]] std::optional<bool> mouthSlotMatchesBeak(const Creature &creature);

} // namespace creatures
