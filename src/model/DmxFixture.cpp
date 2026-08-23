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

} // namespace creatures
