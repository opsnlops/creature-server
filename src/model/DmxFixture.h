
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/namespace-stuffs.h"

namespace creatures {

enum class FixtureType {
    Light,
    SmokeMachine,
    Fogger,
    Generic,
};

std::string fixtureTypeToString(FixtureType type);
FixtureType fixtureTypeFromString(const std::string &str);

struct FixtureChannel {
    uint16_t offset;
    std::string name;
    std::string kind;
};

struct FixturePatternValue {
    std::string channel;
    uint8_t value;
};

struct FixturePattern {
    std::string id;
    std::string name;
    std::vector<FixturePatternValue> values;
    uint32_t fade_in_ms;
    uint32_t fade_out_ms;
    uint32_t hold_ms;
};

struct FixtureBinding {
    std::string creature_id;
    std::optional<std::string> on_reason;
    std::optional<std::string> on_state;
    std::string pattern_id;
};

struct DmxFixture {
    fixtureId_t id;
    std::string name;
    FixtureType type{FixtureType::Generic};
    uint16_t channel_offset{0};
    std::optional<universe_t> assigned_universe;
    std::vector<FixtureChannel> channels;
    std::vector<FixturePattern> patterns;
    std::vector<FixtureBinding> bindings;

    const FixtureChannel *findChannelByName(const std::string &channelName) const;
    const FixturePattern *findPatternById(const std::string &patternId) const;
};

#include OATPP_CODEGEN_BEGIN(DTO)

class FixtureChannelDto : public oatpp::DTO {

    DTO_INIT(FixtureChannelDto, DTO /* extends */)

    DTO_FIELD_INFO(offset) {
        info->description = "Channel offset relative to the fixture's channel_offset. Absolute DMX address = "
                            "fixture.channel_offset + offset.";
    }
    DTO_FIELD(UInt16, offset);

    DTO_FIELD_INFO(name) { info->description = "Channel name (e.g. 'red', 'brightness'). Unique within the fixture."; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(kind) {
        info->description =
            "UI hint for channel type (color_red, color_green, master_dimmer, generic, ...). Free-form.";
        info->required = false;
    }
    DTO_FIELD(String, kind);
};

class FixturePatternValueDto : public oatpp::DTO {

    DTO_INIT(FixturePatternValueDto, DTO /* extends */)

    DTO_FIELD_INFO(channel) { info->description = "Channel name on the parent fixture."; }
    DTO_FIELD(String, channel);

    DTO_FIELD_INFO(value) { info->description = "DMX value [0, 255]."; }
    DTO_FIELD(UInt8, value);
};

class FixturePatternDto : public oatpp::DTO {

    DTO_INIT(FixturePatternDto, DTO /* extends */)

    DTO_FIELD_INFO(id) { info->description = "Pattern UUID. Unique within the fixture."; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(name) { info->description = "Display name for the UI."; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(values) { info->description = "Target channel values when this pattern is held."; }
    DTO_FIELD(List<Object<FixturePatternValueDto>>, values);

    DTO_FIELD_INFO(fade_in_ms) {
        info->description = "Milliseconds to ramp from current to target values. 0 = snap.";
        info->required = false;
    }
    DTO_FIELD(UInt32, fade_in_ms);

    DTO_FIELD_INFO(fade_out_ms) {
        info->description = "Milliseconds to ramp back when the pattern stops. 0 = snap.";
        info->required = false;
    }
    DTO_FIELD(UInt32, fade_out_ms);

    DTO_FIELD_INFO(hold_ms) {
        info->description = "Milliseconds to hold target values after fade-in. 0 = hold until external stop.";
        info->required = false;
    }
    DTO_FIELD(UInt32, hold_ms);
};

class FixtureBindingDto : public oatpp::DTO {

    DTO_INIT(FixtureBindingDto, DTO /* extends */)

    DTO_FIELD_INFO(creature_id) {
        info->description = "UUID of the creature whose activity transitions this binding observes.";
    }
    DTO_FIELD(String, creature_id);

    DTO_FIELD_INFO(on_reason) {
        info->description = "Activity reason filter (play|playlist|ad_hoc|idle|disabled|cancelled|streaming) or null "
                            "for wildcard.";
        info->required = false;
    }
    DTO_FIELD(String, on_reason);

    DTO_FIELD_INFO(on_state) {
        info->description = "Activity state filter (running|idle|disabled|stopped) or null for wildcard.";
        info->required = false;
    }
    DTO_FIELD(String, on_state);

    DTO_FIELD_INFO(pattern_id) { info->description = "UUID of a FixturePattern defined on this same fixture."; }
    DTO_FIELD(String, pattern_id);
};

class DmxFixtureDto : public oatpp::DTO {

    DTO_INIT(DmxFixtureDto, DTO /* extends */)

    DTO_FIELD_INFO(id) { info->description = "Fixture UUID."; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(name) { info->description = "Human-readable display name."; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(type) { info->description = "Fixture category: light, smoke_machine, fogger, or generic. UI hint."; }
    DTO_FIELD(String, type);

    DTO_FIELD_INFO(channel_offset) { info->description = "Starting DMX channel within the universe [0, 511]."; }
    DTO_FIELD(UInt16, channel_offset);

    DTO_FIELD_INFO(assigned_universe) {
        info->description = "Persisted E1.31 universe number. Null = unassigned (no DMX output).";
        info->required = false;
    }
    DTO_FIELD(UInt32, assigned_universe);

    DTO_FIELD_INFO(channels) { info->description = "Addressable channels on this fixture. Non-empty."; }
    DTO_FIELD(List<Object<FixtureChannelDto>>, channels);

    DTO_FIELD_INFO(patterns) {
        info->description = "Named DMX value snapshots that bindings can trigger.";
        info->required = false;
    }
    DTO_FIELD(List<Object<FixturePatternDto>>, patterns);

    DTO_FIELD_INFO(bindings) {
        info->description = "Triggers connecting creature activity transitions to patterns on this fixture.";
        info->required = false;
    }
    DTO_FIELD(List<Object<FixtureBindingDto>>, bindings);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<DmxFixtureDto> convertToDto(const DmxFixture &fixture);
DmxFixture convertFromDto(const std::shared_ptr<DmxFixtureDto> &fixtureDto);

} // namespace creatures
