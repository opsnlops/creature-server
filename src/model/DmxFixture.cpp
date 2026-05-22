
#include <spdlog/spdlog.h>

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>

#include "DmxFixture.h"

namespace creatures {

std::vector<std::string> fixture_required_top_level_fields = {"id", "name", "type", "channel_offset", "channels"};
std::vector<std::string> fixture_required_channel_fields = {"offset", "name"};
std::vector<std::string> fixture_required_pattern_fields = {"id", "name", "values"};
std::vector<std::string> fixture_required_pattern_value_fields = {"channel", "value"};
std::vector<std::string> fixture_required_binding_fields = {"creature_id", "pattern_id"};

std::string fixtureTypeToString(FixtureType type) {
    switch (type) {
    case FixtureType::Light:
        return "light";
    case FixtureType::SmokeMachine:
        return "smoke_machine";
    case FixtureType::Fogger:
        return "fogger";
    case FixtureType::Generic:
        return "generic";
    }
    return "generic";
}

FixtureType fixtureTypeFromString(const std::string &str) {
    if (str == "light")
        return FixtureType::Light;
    if (str == "smoke_machine")
        return FixtureType::SmokeMachine;
    if (str == "fogger")
        return FixtureType::Fogger;
    if (str == "generic")
        return FixtureType::Generic;
    warn("Unknown fixture type '{}', defaulting to Generic", str);
    return FixtureType::Generic;
}

const FixtureChannel *DmxFixture::findChannelByName(const std::string &channelName) const {
    for (const auto &ch : channels) {
        if (ch.name == channelName)
            return &ch;
    }
    return nullptr;
}

const FixturePattern *DmxFixture::findPatternById(const std::string &patternId) const {
    for (const auto &p : patterns) {
        if (p.id == patternId)
            return &p;
    }
    return nullptr;
}

DmxFixture convertFromDto(const std::shared_ptr<DmxFixtureDto> &fixtureDto) {

    debug("Converting DmxFixtureDto to DmxFixture");

    DmxFixture fixture;
    fixture.id = fixtureDto->id;
    fixture.name = fixtureDto->name;
    fixture.type = fixtureTypeFromString(fixtureDto->type);
    fixture.channel_offset = fixtureDto->channel_offset;

    if (fixtureDto->assigned_universe) {
        fixture.assigned_universe = *fixtureDto->assigned_universe;
    }

    if (fixtureDto->channels) {
        for (const auto &channelDto : *fixtureDto->channels) {
            if (!channelDto)
                continue;
            FixtureChannel ch;
            ch.offset = channelDto->offset;
            ch.name = channelDto->name;
            ch.kind = channelDto->kind ? std::string(channelDto->kind) : std::string("generic");
            fixture.channels.push_back(ch);
        }
    }

    if (fixtureDto->patterns) {
        for (const auto &patternDto : *fixtureDto->patterns) {
            if (!patternDto)
                continue;
            FixturePattern p;
            p.id = patternDto->id;
            p.name = patternDto->name;
            p.fade_in_ms = patternDto->fade_in_ms ? static_cast<uint32_t>(*patternDto->fade_in_ms) : 0u;
            p.fade_out_ms = patternDto->fade_out_ms ? static_cast<uint32_t>(*patternDto->fade_out_ms) : 0u;
            p.hold_ms = patternDto->hold_ms ? static_cast<uint32_t>(*patternDto->hold_ms) : 0u;
            if (patternDto->values) {
                for (const auto &valueDto : *patternDto->values) {
                    if (!valueDto)
                        continue;
                    FixturePatternValue v;
                    v.channel = valueDto->channel;
                    v.value = valueDto->value;
                    p.values.push_back(v);
                }
            }
            fixture.patterns.push_back(p);
        }
    }

    if (fixtureDto->bindings) {
        for (const auto &bindingDto : *fixtureDto->bindings) {
            if (!bindingDto)
                continue;
            FixtureBinding b;
            b.creature_id = bindingDto->creature_id;
            b.pattern_id = bindingDto->pattern_id;
            if (bindingDto->on_reason) {
                b.on_reason = std::string(bindingDto->on_reason);
            }
            if (bindingDto->on_state) {
                b.on_state = std::string(bindingDto->on_state);
            }
            fixture.bindings.push_back(b);
        }
    }

    return fixture;
}

oatpp::Object<DmxFixtureDto> convertToDto(const DmxFixture &fixture) {
    auto fixtureDto = DmxFixtureDto::createShared();
    fixtureDto->id = fixture.id;
    fixtureDto->name = fixture.name;
    fixtureDto->type = fixtureTypeToString(fixture.type);
    fixtureDto->channel_offset = fixture.channel_offset;

    if (fixture.assigned_universe.has_value()) {
        fixtureDto->assigned_universe = *fixture.assigned_universe;
    }

    fixtureDto->channels = oatpp::List<oatpp::Object<FixtureChannelDto>>::createShared();
    for (const auto &ch : fixture.channels) {
        auto channelDto = FixtureChannelDto::createShared();
        channelDto->offset = ch.offset;
        channelDto->name = ch.name;
        channelDto->kind = ch.kind;
        fixtureDto->channels->push_back(channelDto);
    }

    if (!fixture.patterns.empty()) {
        auto patternsList = oatpp::List<oatpp::Object<FixturePatternDto>>::createShared();
        for (const auto &p : fixture.patterns) {
            auto patternDto = FixturePatternDto::createShared();
            patternDto->id = p.id;
            patternDto->name = p.name;
            patternDto->fade_in_ms = p.fade_in_ms;
            patternDto->fade_out_ms = p.fade_out_ms;
            patternDto->hold_ms = p.hold_ms;
            patternDto->values = oatpp::List<oatpp::Object<FixturePatternValueDto>>::createShared();
            for (const auto &v : p.values) {
                auto valueDto = FixturePatternValueDto::createShared();
                valueDto->channel = v.channel;
                valueDto->value = v.value;
                patternDto->values->push_back(valueDto);
            }
            patternsList->push_back(patternDto);
        }
        fixtureDto->patterns = patternsList;
    }

    if (!fixture.bindings.empty()) {
        auto bindingsList = oatpp::List<oatpp::Object<FixtureBindingDto>>::createShared();
        for (const auto &b : fixture.bindings) {
            auto bindingDto = FixtureBindingDto::createShared();
            bindingDto->creature_id = b.creature_id;
            bindingDto->pattern_id = b.pattern_id;
            if (b.on_reason.has_value()) {
                bindingDto->on_reason = *b.on_reason;
            }
            if (b.on_state.has_value()) {
                bindingDto->on_state = *b.on_state;
            }
            bindingsList->push_back(bindingDto);
        }
        fixtureDto->bindings = bindingsList;
    }

    return fixtureDto;
}

} // namespace creatures
