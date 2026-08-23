#pragma once

#include <memory>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/Creature.h"
#include "server/ws/dto/InputDto.h"

namespace creatures {

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
    DTO_FIELD(String, state);
    DTO_FIELD_INFO(animation_id) { info->description = "Current animation ID if applicable"; }
    DTO_FIELD(String, animation_id);
    DTO_FIELD_INFO(session_id) { info->description = "Session UUID for the current activity"; }
    DTO_FIELD(String, session_id);
    DTO_FIELD_INFO(reason) { info->description = "Reason for this activity"; }
    DTO_FIELD(String, reason);
    DTO_FIELD_INFO(started_at) { info->description = "Activity start time (ISO8601)"; }
    DTO_FIELD(String, started_at);
    DTO_FIELD_INFO(updated_at) { info->description = "Last update time (ISO8601)"; }
    DTO_FIELD(String, updated_at);
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
    DTO_INIT(GazeAxisDto, DTO)
    DTO_FIELD_INFO(input) { info->description = "Name of the input this axis drives"; }
    DTO_FIELD(String, input);
    DTO_FIELD_INFO(degrees_at_min) { info->description = "Angle in degrees when the input byte is 0"; }
    DTO_FIELD(Float32, degrees_at_min);
    DTO_FIELD_INFO(degrees_at_max) { info->description = "Angle in degrees when the input byte is 255"; }
    DTO_FIELD(Float32, degrees_at_max);
    DTO_FIELD_INFO(listening_amount) { info->required = false; }
    DTO_FIELD(Float32, listening_amount);
};

class GazeConfigDto : public oatpp::DTO {
    DTO_INIT(GazeConfigDto, DTO)
    DTO_FIELD_INFO(pan) { info->description = "Left/right head turn"; }
    DTO_FIELD(Object<GazeAxisDto>, pan);
    DTO_FIELD_INFO(elevation) { info->description = "Up/down head aim"; }
    DTO_FIELD(Object<GazeAxisDto>, elevation);
    DTO_FIELD_INFO(cock) { info->description = "Expressive sideways head tilt while listening"; }
    DTO_FIELD(Object<GazeAxisDto>, cock);
};

class CreatureDto : public oatpp::DTO {
    DTO_INIT(CreatureDto, DTO)
    DTO_FIELD_INFO(id) { info->description = "Creature ID"; }
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
        info->description = "Name of the input that drives the mouth";
        info->required = false;
    }
    DTO_FIELD(String, mouth_input);
    DTO_FIELD_INFO(inputs) { info->description = "The input map for this creature"; }
    DTO_FIELD(List<Object<InputDto>>, inputs);
    DTO_FIELD_INFO(speech_loop_animation_ids) {
        info->description = "Animations used as base speech loops";
        info->required = false;
    }
    DTO_FIELD(List<String>, speech_loop_animation_ids);
    DTO_FIELD_INFO(idle_animation_ids) {
        info->description = "Animations used as idle loops";
        info->required = false;
    }
    DTO_FIELD(List<String>, idle_animation_ids);
    DTO_FIELD_INFO(gaze) {
        info->description = "Which inputs the gaze layer may drive";
        info->required = false;
    }
    DTO_FIELD(Object<GazeConfigDto>, gaze);
    DTO_FIELD_INFO(runtime) {
        info->description = "Runtime state, absent in config documents";
        info->required = false;
    }
    DTO_FIELD(Object<CreatureRuntimeDto>, runtime);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<CreatureDto> convertToDto(const Creature &creature);
Creature convertFromDto(const std::shared_ptr<CreatureDto> &creatureDto);

} // namespace creatures
