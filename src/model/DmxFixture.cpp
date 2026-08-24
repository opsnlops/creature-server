#include "model/DmxFixture.h"

#include <algorithm>
#include <set>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "model/JsonCodec.h"
#include "util/helpers.h"

namespace creatures {

namespace {

constexpr std::size_t MAX_CHANNELS_PER_FIXTURE = 64;
constexpr std::size_t MAX_PATTERNS_PER_FIXTURE = 256;
constexpr std::size_t MAX_VALUES_PER_PATTERN = 64;
constexpr std::size_t MAX_BINDINGS_PER_FIXTURE = 256;
constexpr std::size_t MAX_FIXTURE_NAME_BYTES = 128;
constexpr std::size_t MAX_CHANNEL_NAME_BYTES = 64;
constexpr std::size_t MAX_CHANNEL_KIND_BYTES = 64;

template <typename T> Result<DmxFixture> forwardFixtureError(const Result<T> &result) {
    return Result<DmxFixture>{result.getError().value()};
}

template <typename T> Result<DmxFixture> invalidFixture(T &&message) {
    return json_codec::invalid<DmxFixture>(std::forward<T>(message));
}

} // namespace

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

Result<DmxFixture> dmxFixtureFromJson(const nlohmann::json &json, std::string_view path) {
    try {
        auto fields = json_codec::rejectUnknownFields(
            json, path,
            {"id", "name", "type", "channel_offset", "assigned_universe", "channels", "patterns", "bindings"});
        if (!fields.isSuccess())
            return forwardFixtureError(fields);

        auto id = json_codec::requiredString(json, path, "id", 36);
        auto name = json_codec::requiredString(json, path, "name", MAX_FIXTURE_NAME_BYTES);
        auto type = json_codec::requiredString(json, path, "type", 32);
        auto channelOffset = json_codec::requiredUnsigned<uint16_t>(json, path, "channel_offset", 511);
        auto universe = json_codec::optionalUnsigned<universe_t>(json, path, "assigned_universe", 63999, true);
        auto channels = json_codec::requiredArray(json, path, "channels", MAX_CHANNELS_PER_FIXTURE, 1);
        if (!id.isSuccess())
            return forwardFixtureError(id);
        if (!name.isSuccess())
            return forwardFixtureError(name);
        if (!type.isSuccess())
            return forwardFixtureError(type);
        if (!channelOffset.isSuccess())
            return forwardFixtureError(channelOffset);
        if (!universe.isSuccess())
            return forwardFixtureError(universe);
        if (!channels.isSuccess())
            return forwardFixtureError(channels);
        if (!isUuidShape(id.getValue().value()))
            return invalidFixture(fmt::format("{}.id must be a UUID", path));
        if (universe.getValue().value() && *universe.getValue().value() < 1)
            return invalidFixture(fmt::format("{}.assigned_universe must be at least 1", path));

        DmxFixture fixture;
        fixture.id = id.getValue().value();
        fixture.name = name.getValue().value();
        fixture.type = fixtureTypeFromString(type.getValue().value());
        fixture.channel_offset = channelOffset.getValue().value();
        fixture.assigned_universe = universe.getValue().value();

        std::set<std::string> channelNames;
        uint16_t maxOffset = 0;
        const auto &channelArray = channels.getValue()->get();
        fixture.channels.reserve(channelArray.size());
        for (std::size_t index = 0; index < channelArray.size(); ++index) {
            const auto entryPath = fmt::format("{}.channels[{}]", path, index);
            const auto &entry = channelArray[index];
            auto entryFields = json_codec::rejectUnknownFields(entry, entryPath, {"offset", "name", "kind"});
            auto offset = json_codec::requiredUnsigned<uint16_t>(entry, entryPath, "offset", 511);
            auto channelName = json_codec::requiredString(entry, entryPath, "name", MAX_CHANNEL_NAME_BYTES);
            auto kind = json_codec::optionalString(entry, entryPath, "kind", MAX_CHANNEL_KIND_BYTES, false, true);
            if (!entryFields.isSuccess())
                return forwardFixtureError(entryFields);
            if (!offset.isSuccess())
                return forwardFixtureError(offset);
            if (!channelName.isSuccess())
                return forwardFixtureError(channelName);
            if (!kind.isSuccess())
                return forwardFixtureError(kind);
            if (!channelNames.emplace(channelName.getValue().value()).second)
                return invalidFixture(fmt::format("{}.name duplicates an earlier channel", entryPath));
            maxOffset = std::max(maxOffset, offset.getValue().value());
            fixture.channels.push_back({offset.getValue().value(), channelName.getValue().value(),
                                        kind.getValue().value().value_or("generic")});
        }
        if (static_cast<uint32_t>(fixture.channel_offset) + maxOffset > 511)
            return invalidFixture(
                fmt::format("{}.channel_offset plus maximum channel offset must not exceed 511", path));

        const auto patternsIt = json.find("patterns");
        if (patternsIt != json.end() && !patternsIt->is_null()) {
            if (!patternsIt->is_array() || patternsIt->size() > MAX_PATTERNS_PER_FIXTURE)
                return invalidFixture(fmt::format("{}.patterns must be an array with at most {} entries", path,
                                                  MAX_PATTERNS_PER_FIXTURE));
            std::set<std::string> patternIds;
            fixture.patterns.reserve(patternsIt->size());
            for (std::size_t index = 0; index < patternsIt->size(); ++index) {
                const auto entryPath = fmt::format("{}.patterns[{}]", path, index);
                const auto &entry = (*patternsIt)[index];
                auto entryFields = json_codec::rejectUnknownFields(
                    entry, entryPath, {"id", "name", "values", "fade_in_ms", "fade_out_ms", "hold_ms"});
                auto patternId = json_codec::requiredString(entry, entryPath, "id", 36);
                auto patternName = json_codec::requiredString(entry, entryPath, "name", MAX_FIXTURE_NAME_BYTES);
                auto values = json_codec::requiredArray(entry, entryPath, "values", MAX_VALUES_PER_PATTERN);
                auto fadeIn = json_codec::optionalUnsigned<uint32_t>(entry, entryPath, "fade_in_ms");
                auto fadeOut = json_codec::optionalUnsigned<uint32_t>(entry, entryPath, "fade_out_ms");
                auto hold = json_codec::optionalUnsigned<uint32_t>(entry, entryPath, "hold_ms");
                if (!entryFields.isSuccess() || !patternId.isSuccess() || !patternName.isSuccess() ||
                    !values.isSuccess() || !fadeIn.isSuccess() || !fadeOut.isSuccess() || !hold.isSuccess())
                    return invalidFixture(fmt::format("{} is invalid", entryPath));
                if (!isUuidShape(patternId.getValue().value()))
                    return invalidFixture(fmt::format("{}.id must be a UUID", entryPath));
                if (!patternIds.emplace(patternId.getValue().value()).second)
                    return invalidFixture(fmt::format("{}.id duplicates an earlier pattern", entryPath));
                FixturePattern pattern{patternId.getValue().value(),
                                       patternName.getValue().value(),
                                       {},
                                       fadeIn.getValue().value().value_or(0),
                                       fadeOut.getValue().value().value_or(0),
                                       hold.getValue().value().value_or(0)};
                std::set<std::string> valueChannels;
                for (std::size_t valueIndex = 0; valueIndex < values.getValue()->get().size(); ++valueIndex) {
                    const auto valuePath = fmt::format("{}.values[{}]", entryPath, valueIndex);
                    const auto &value = values.getValue()->get()[valueIndex];
                    auto valueFields = json_codec::rejectUnknownFields(value, valuePath, {"channel", "value"});
                    auto channel = json_codec::requiredString(value, valuePath, "channel", MAX_CHANNEL_NAME_BYTES);
                    auto dmxValue = json_codec::requiredUnsigned<uint8_t>(value, valuePath, "value", 255);
                    if (!valueFields.isSuccess() || !channel.isSuccess() || !dmxValue.isSuccess())
                        return invalidFixture(fmt::format("{} is invalid", valuePath));
                    if (!channelNames.contains(channel.getValue().value()))
                        return invalidFixture(fmt::format("{}.channel must name a fixture channel", valuePath));
                    if (!valueChannels.emplace(channel.getValue().value()).second)
                        return invalidFixture(fmt::format("{}.channel duplicates an earlier value", valuePath));
                    pattern.values.push_back({channel.getValue().value(), dmxValue.getValue().value()});
                }
                fixture.patterns.push_back(std::move(pattern));
            }
        }

        const auto bindingsIt = json.find("bindings");
        if (bindingsIt != json.end() && !bindingsIt->is_null()) {
            if (!bindingsIt->is_array() || bindingsIt->size() > MAX_BINDINGS_PER_FIXTURE)
                return invalidFixture(fmt::format("{}.bindings must be an array with at most {} entries", path,
                                                  MAX_BINDINGS_PER_FIXTURE));
            std::set<std::string> patternIds;
            for (const auto &pattern : fixture.patterns)
                patternIds.insert(pattern.id);
            fixture.bindings.reserve(bindingsIt->size());
            for (std::size_t index = 0; index < bindingsIt->size(); ++index) {
                const auto entryPath = fmt::format("{}.bindings[{}]", path, index);
                const auto &entry = (*bindingsIt)[index];
                auto entryFields = json_codec::rejectUnknownFields(
                    entry, entryPath, {"creature_id", "on_reason", "on_state", "pattern_id"});
                auto creatureId = json_codec::requiredString(entry, entryPath, "creature_id", 36);
                auto patternId = json_codec::requiredString(entry, entryPath, "pattern_id", 36);
                auto reason = json_codec::optionalString(entry, entryPath, "on_reason", 32, false, true);
                auto state = json_codec::optionalString(entry, entryPath, "on_state", 32, false, true);
                if (!entryFields.isSuccess() || !creatureId.isSuccess() || !patternId.isSuccess() ||
                    !reason.isSuccess() || !state.isSuccess())
                    return invalidFixture(fmt::format("{} is invalid", entryPath));
                if (!isUuidShape(creatureId.getValue().value()) || !isUuidShape(patternId.getValue().value()))
                    return invalidFixture(fmt::format("{}.creature_id and pattern_id must be UUIDs", entryPath));
                if (!patternIds.contains(patternId.getValue().value()))
                    return invalidFixture(fmt::format("{}.pattern_id must name a fixture pattern", entryPath));
                static const std::set<std::string> validReasons = {"play",     "playlist",  "ad_hoc",   "idle",
                                                                   "disabled", "cancelled", "streaming"};
                static const std::set<std::string> validStates = {"running", "idle", "disabled", "stopped"};
                if (reason.getValue().value() && !validReasons.contains(*reason.getValue().value()))
                    return invalidFixture(fmt::format("{}.on_reason is not a known activity reason", entryPath));
                if (state.getValue().value() && !validStates.contains(*state.getValue().value()))
                    return invalidFixture(fmt::format("{}.on_state is not a known activity state", entryPath));
                fixture.bindings.push_back({creatureId.getValue().value(), reason.getValue().value(),
                                            state.getValue().value(), patternId.getValue().value()});
            }
        }
        return Result<DmxFixture>{fixture};
    } catch (const nlohmann::json::exception &error) {
        return invalidFixture(fmt::format("{} is invalid JSON: {}", path, error.what()));
    }
}

} // namespace creatures
