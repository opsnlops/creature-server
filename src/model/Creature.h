
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "Input.h"

#include "server/namespace-stuffs.h"
#include "server/ws/dto/InputDto.h"

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

#include OATPP_CODEGEN_BEGIN(DTO)

class CreatureRuntimeErrorDto : public oatpp::DTO {

    DTO_INIT(CreatureRuntimeErrorDto, DTO)

    DTO_FIELD_INFO(message) { info->description = "Last runtime error message for this creature"; }
    DTO_FIELD(String, message);

    DTO_FIELD_INFO(timestamp) { info->description = "When the error occurred (ISO8601)"; }
    DTO_FIELD(String, timestamp);
};

class CreatureRuntimeCountersDto : public oatpp::DTO {

    DTO_INIT(CreatureRuntimeCountersDto, DTO)

    DTO_FIELD_INFO(sessions_started_total) { info->description = "Total sessions started for this creature"; }
    DTO_FIELD(UInt64, sessions_started_total);

    DTO_FIELD_INFO(sessions_cancelled_total) { info->description = "Total sessions cancelled for this creature"; }
    DTO_FIELD(UInt64, sessions_cancelled_total);

    DTO_FIELD_INFO(idle_started_total) { info->description = "Total idle sessions started"; }
    DTO_FIELD(UInt64, idle_started_total);

    DTO_FIELD_INFO(idle_stopped_total) { info->description = "Total idle sessions stopped"; }
    DTO_FIELD(UInt64, idle_stopped_total);

    DTO_FIELD_INFO(idle_toggles_total) { info->description = "Total idle enable/disable toggles"; }
    DTO_FIELD(UInt64, idle_toggles_total);

    DTO_FIELD_INFO(skips_missing_creature_total) {
        info->description = "Total times this creature was skipped due to missing runtime availability";
    }
    DTO_FIELD(UInt64, skips_missing_creature_total);

    DTO_FIELD_INFO(bgm_takeovers_total) { info->description = "Total times BGM ownership changed to this creature"; }
    DTO_FIELD(UInt64, bgm_takeovers_total);

    DTO_FIELD_INFO(audio_resets_total) { info->description = "Total audio encoder resets for this creature"; }
    DTO_FIELD(UInt64, audio_resets_total);
};

class CreatureRuntimeActivityDto : public oatpp::DTO {

    DTO_INIT(CreatureRuntimeActivityDto, DTO)

    DTO_FIELD_INFO(state) { info->description = "Current activity state for this creature"; }
    DTO_FIELD(String, state); // running|idle|disabled|stopped

    DTO_FIELD_INFO(animation_id) { info->description = "Current animation ID if applicable"; }
    DTO_FIELD(String, animation_id); // nullable

    DTO_FIELD_INFO(session_id) { info->description = "Session UUID for the current activity"; }
    DTO_FIELD(String, session_id); // nullable UUIDv4

    DTO_FIELD_INFO(reason) { info->description = "Reason for this activity (play|ad_hoc|playlist|idle|disabled)"; }
    DTO_FIELD(String, reason); // nullable

    DTO_FIELD_INFO(started_at) { info->description = "Activity start time (ISO8601)"; }
    DTO_FIELD(String, started_at); // nullable

    DTO_FIELD_INFO(updated_at) { info->description = "Last update time (ISO8601)"; }
    DTO_FIELD(String, updated_at); // nullable
};

class CreatureRuntimeDto : public oatpp::DTO {

    DTO_INIT(CreatureRuntimeDto, DTO)

    DTO_FIELD_INFO(idle_enabled) { info->description = "Whether idle loop is enabled for this creature"; }
    DTO_FIELD(Boolean, idle_enabled);

    DTO_FIELD_INFO(activity) { info->description = "Current runtime activity for this creature"; }
    DTO_FIELD(Object<CreatureRuntimeActivityDto>, activity);

    DTO_FIELD_INFO(counters) { info->description = "Runtime counters for this creature"; }
    DTO_FIELD(Object<CreatureRuntimeCountersDto>, counters);

    DTO_FIELD_INFO(bgm_owner) { info->description = "Creature ID that currently owns BGM, if any"; }
    DTO_FIELD(String, bgm_owner);

    DTO_FIELD_INFO(last_error) { info->description = "Last runtime error for this creature, if any"; }
    DTO_FIELD(Object<CreatureRuntimeErrorDto>, last_error);
};

class GazeAxisDto : public oatpp::DTO {

    DTO_INIT(GazeAxisDto, DTO /* extends */)

    DTO_FIELD_INFO(input) {
        info->description = "Name of the entry in `inputs` this axis drives, e.g. 'neck_rotate' or 'head_height'. "
                            "The slot is resolved from this name, since input layouts differ between creatures.";
    }
    DTO_FIELD(String, input);

    DTO_FIELD_INFO(degrees_at_min) { info->description = "Angle in degrees when the input byte is 0"; }
    DTO_FIELD(Float32, degrees_at_min);

    DTO_FIELD_INFO(degrees_at_max) {
        info->description = "Angle in degrees when the input byte is 255. Set this LOWER than degrees_at_min to "
                            "express a reversed axis.";
    }
    DTO_FIELD(Float32, degrees_at_max);

    DTO_FIELD_INFO(listening_amount) {
        info->description = "Cock axis only: fraction of the available range a listening creature may tilt its head "
                            "by. 0 disables the head-cock.";
        info->required = false;
    }
    DTO_FIELD(Float32, listening_amount);
};

class GazeConfigDto : public oatpp::DTO {

    DTO_INIT(GazeConfigDto, DTO /* extends */)

    DTO_FIELD_INFO(pan) { info->description = "Left/right head turn. Normally the 'neck_rotate' input."; }
    DTO_FIELD(Object<GazeAxisDto>, pan);

    DTO_FIELD_INFO(elevation) {
        info->description = "Up/down head aim, for looking at a taller or shorter creature. Normally the "
                            "'head_height' input — NOT 'head_tilt', which cocks the head sideways instead.";
    }
    DTO_FIELD(Object<GazeAxisDto>, elevation);

    DTO_FIELD_INFO(cock) {
        info->description = "Expressive sideways head tilt while listening. Normally the 'head_tilt' input. Aims at "
                            "nothing; it just makes a listening creature look attentive.";
    }
    DTO_FIELD(Object<GazeAxisDto>, cock);
};

class CreatureDto : public oatpp::DTO {

    DTO_INIT(CreatureDto, DTO /* extends */)

    DTO_FIELD_INFO(id) { info->description = "Creature ID in the form of a MongoDB OID"; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(name) { info->description = "The creature's name"; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(channel_offset) {
        info->description = "The offset of the channel for this creature in the universe";
    }
    DTO_FIELD(UInt16, channel_offset);

    DTO_FIELD_INFO(audio_channel) { info->description = "The audio channel for this creature"; }
    DTO_FIELD(UInt16, audio_channel);

    DTO_FIELD_INFO(mouth_slot) {
        info->description = "The slot in the motion array that corresponds to the creature's mouth";
        info->required = true;
    }
    DTO_FIELD(UInt8, mouth_slot);

    DTO_FIELD_INFO(mouth_input) {
        info->description = "Name of the input that drives the mouth, normally 'beak'. Preferred over mouth_slot: "
                            "the slot is resolved per creature, since input layouts differ between creatures.";
        info->required = false;
    }
    DTO_FIELD(String, mouth_input);

    DTO_FIELD_INFO(inputs) { info->description = "The input map for this creature"; }
    DTO_FIELD(List<Object<InputDto>>, inputs);

    DTO_FIELD_INFO(speech_loop_animation_ids) {
        info->description = "Animations that can be used as base speech loops for ad-hoc speech";
        info->required = false;
    }
    DTO_FIELD(List<String>, speech_loop_animation_ids);

    DTO_FIELD_INFO(idle_animation_ids) {
        info->description = "Animations that can be used as idle loops for this creature";
        info->required = false;
    }
    DTO_FIELD(List<String>, idle_animation_ids);

    DTO_FIELD_INFO(gaze) {
        info->description = "Which inputs the gaze layer may drive, and their angular ranges. Absent means this "
                            "creature never turns to look at anything.";
        info->required = false;
    }
    DTO_FIELD(Object<GazeConfigDto>, gaze);

    DTO_FIELD_INFO(runtime) {
        info->description = "Runtime state (present only at runtime; absent in config documents)";
        info->required = false;
    }
    DTO_FIELD(Object<CreatureRuntimeDto>, runtime);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<CreatureDto> convertToDto(const Creature &creature);
creatures::Creature convertFromDto(const std::shared_ptr<CreatureDto> &creatureDto);

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
