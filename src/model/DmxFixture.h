#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "server/namespace-stuffs.h"

namespace creatures {
enum class FixtureType { Light, SmokeMachine, Fogger, Generic };
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
} // namespace creatures
