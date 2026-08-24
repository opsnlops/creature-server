#include "model/DmxFixture.h"

#include <spdlog/spdlog.h>

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

FixtureType fixtureTypeFromString(const std::string &value) {
    if (value == "light")
        return FixtureType::Light;
    if (value == "smoke_machine")
        return FixtureType::SmokeMachine;
    if (value == "fogger")
        return FixtureType::Fogger;
    if (value != "generic")
        warn("Unknown fixture type '{}', defaulting to Generic", value);
    return FixtureType::Generic;
}

const FixtureChannel *DmxFixture::findChannelByName(const std::string &channelName) const {
    for (const auto &channel : channels)
        if (channel.name == channelName)
            return &channel;
    return nullptr;
}

const FixturePattern *DmxFixture::findPatternById(const std::string &patternId) const {
    for (const auto &pattern : patterns)
        if (pattern.id == patternId)
            return &pattern;
    return nullptr;
}

nlohmann::json dmxFixtureToJson(const DmxFixture &fixture) {
    nlohmann::json channels = nlohmann::json::array();
    for (const auto &channel : fixture.channels) {
        channels.push_back({{"offset", channel.offset}, {"name", channel.name}, {"kind", channel.kind}});
    }

    nlohmann::json patterns = nlohmann::json::array();
    for (const auto &pattern : fixture.patterns) {
        nlohmann::json values = nlohmann::json::array();
        for (const auto &value : pattern.values) {
            values.push_back({{"channel", value.channel}, {"value", value.value}});
        }
        patterns.push_back({{"id", pattern.id},
                            {"name", pattern.name},
                            {"values", std::move(values)},
                            {"fade_in_ms", pattern.fade_in_ms},
                            {"fade_out_ms", pattern.fade_out_ms},
                            {"hold_ms", pattern.hold_ms}});
    }

    nlohmann::json bindings = nlohmann::json::array();
    for (const auto &binding : fixture.bindings) {
        nlohmann::json value = {{"creature_id", binding.creature_id}, {"pattern_id", binding.pattern_id}};
        if (binding.on_reason) {
            value["on_reason"] = *binding.on_reason;
        }
        if (binding.on_state) {
            value["on_state"] = *binding.on_state;
        }
        bindings.push_back(std::move(value));
    }

    nlohmann::json result = {{"id", fixture.id},
                             {"name", fixture.name},
                             {"type", fixtureTypeToString(fixture.type)},
                             {"channel_offset", fixture.channel_offset},
                             {"channels", std::move(channels)},
                             {"patterns", std::move(patterns)},
                             {"bindings", std::move(bindings)}};
    if (fixture.assigned_universe) {
        result["assigned_universe"] = *fixture.assigned_universe;
    }
    return result;
}

} // namespace creatures
