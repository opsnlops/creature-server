#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "model/JsonCodec.h"
#include "server/namespace-stuffs.h"

namespace creatures::api {

inline constexpr std::size_t MAX_FIXTURE_CONTROL_REQUEST_BODY_BYTES = 4096;
inline constexpr std::size_t MAX_FIXTURE_CONFIG_REQUEST_BODY_BYTES = 1024 * 1024;
inline constexpr std::size_t MAX_FIXTURE_CONTROL_VALUES = 512;
inline constexpr std::size_t MAX_FIXTURE_CHANNEL_NAME_BYTES = 128;
inline constexpr uint32_t MAX_FIXTURE_CONTROL_DURATION_MS = 10 * 60 * 1000;
inline constexpr universe_t MIN_FIXTURE_UNIVERSE = 1;
inline constexpr universe_t MAX_FIXTURE_UNIVERSE = 63999;

struct SetFixtureUniverseRequest {
    universe_t universe;
};

struct TriggerFixturePatternRequest {
    std::optional<uint32_t> stopAfterMs;
};

struct FixtureChannelValue {
    std::string channel;
    uint8_t value;
};

struct PreviewFixturePatternRequest {
    std::vector<FixtureChannelValue> values;
    uint32_t fadeInMs{0};
    uint32_t fadeOutMs{0};
    uint32_t holdMs{0};
    std::optional<uint32_t> stopAfterMs;
};

struct SetFixtureLiveRequest {
    std::vector<FixtureChannelValue> values;
    uint32_t timeoutMs;
};

inline Result<SetFixtureUniverseRequest> setFixtureUniverseRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "fixture universe request", {"universe"});
    if (!fields.isSuccess())
        return Result<SetFixtureUniverseRequest>{fields.getError().value()};
    auto universe =
        json_codec::requiredUnsigned<universe_t>(json, "fixture universe request", "universe", MAX_FIXTURE_UNIVERSE);
    if (!universe.isSuccess())
        return Result<SetFixtureUniverseRequest>{universe.getError().value()};
    if (universe.getValue().value() < MIN_FIXTURE_UNIVERSE)
        return json_codec::invalid<SetFixtureUniverseRequest>("fixture universe request.universe must be at least 1");
    return Result<SetFixtureUniverseRequest>{{universe.getValue().value()}};
}

inline Result<std::optional<uint32_t>> fixtureStopAfterMsFromJson(const nlohmann::json &json, std::string_view path) {
    auto stopAfter =
        json_codec::optionalUnsigned<uint32_t>(json, path, "stop_after_ms", MAX_FIXTURE_CONTROL_DURATION_MS);
    if (!stopAfter.isSuccess())
        return Result<std::optional<uint32_t>>{stopAfter.getError().value()};
    if (stopAfter.getValue().value().has_value() && *stopAfter.getValue().value() == 0)
        return json_codec::invalid<std::optional<uint32_t>>(
            fmt::format("{}.stop_after_ms must be greater than 0 when provided", path));
    return stopAfter;
}

inline Result<TriggerFixturePatternRequest> triggerFixturePatternRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "fixture pattern trigger request", {"stop_after_ms"});
    if (!fields.isSuccess())
        return Result<TriggerFixturePatternRequest>{fields.getError().value()};
    auto stopAfter = fixtureStopAfterMsFromJson(json, "fixture pattern trigger request");
    if (!stopAfter.isSuccess())
        return Result<TriggerFixturePatternRequest>{stopAfter.getError().value()};
    return Result<TriggerFixturePatternRequest>{{stopAfter.getValue().value()}};
}

inline Result<std::vector<FixtureChannelValue>> fixtureChannelValuesFromJson(const nlohmann::json &json,
                                                                             std::string_view path) {
    auto values = json_codec::requiredArray(json, path, "values", MAX_FIXTURE_CONTROL_VALUES, 1);
    if (!values.isSuccess())
        return Result<std::vector<FixtureChannelValue>>{values.getError().value()};

    std::vector<FixtureChannelValue> parsed;
    parsed.reserve(values.getValue()->get().size());
    std::unordered_set<std::string> channelNames;
    for (std::size_t index = 0; index < values.getValue()->get().size(); ++index) {
        const auto &value = values.getValue()->get().at(index);
        const auto valuePath = fmt::format("{}.values[{}]", path, index);
        auto fields = json_codec::rejectUnknownFields(value, valuePath, {"channel", "value"});
        if (!fields.isSuccess())
            return Result<std::vector<FixtureChannelValue>>{fields.getError().value()};
        auto channel = json_codec::requiredString(value, valuePath, "channel", MAX_FIXTURE_CHANNEL_NAME_BYTES);
        auto dmxValue = json_codec::requiredUnsigned<uint8_t>(value, valuePath, "value", UINT8_MAX);
        if (!channel.isSuccess())
            return Result<std::vector<FixtureChannelValue>>{channel.getError().value()};
        if (!dmxValue.isSuccess())
            return Result<std::vector<FixtureChannelValue>>{dmxValue.getError().value()};
        if (!channelNames.insert(channel.getValue().value()).second) {
            return json_codec::invalid<std::vector<FixtureChannelValue>>(
                fmt::format("{}.channel must not be duplicated", valuePath));
        }
        parsed.push_back({channel.getValue().value(), dmxValue.getValue().value()});
    }
    return Result<std::vector<FixtureChannelValue>>{std::move(parsed)};
}

inline Result<PreviewFixturePatternRequest> previewFixturePatternRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "fixture pattern preview request",
                                                  {"values", "fade_in_ms", "fade_out_ms", "hold_ms", "stop_after_ms"});
    if (!fields.isSuccess())
        return Result<PreviewFixturePatternRequest>{fields.getError().value()};
    auto values = fixtureChannelValuesFromJson(json, "fixture pattern preview request");
    auto fadeIn = json_codec::optionalUnsigned<uint32_t>(json, "fixture pattern preview request", "fade_in_ms",
                                                         MAX_FIXTURE_CONTROL_DURATION_MS);
    auto fadeOut = json_codec::optionalUnsigned<uint32_t>(json, "fixture pattern preview request", "fade_out_ms",
                                                          MAX_FIXTURE_CONTROL_DURATION_MS);
    auto hold = json_codec::optionalUnsigned<uint32_t>(json, "fixture pattern preview request", "hold_ms",
                                                       MAX_FIXTURE_CONTROL_DURATION_MS);
    auto stopAfter = fixtureStopAfterMsFromJson(json, "fixture pattern preview request");
    if (!values.isSuccess())
        return Result<PreviewFixturePatternRequest>{values.getError().value()};
    if (!fadeIn.isSuccess())
        return Result<PreviewFixturePatternRequest>{fadeIn.getError().value()};
    if (!fadeOut.isSuccess())
        return Result<PreviewFixturePatternRequest>{fadeOut.getError().value()};
    if (!hold.isSuccess())
        return Result<PreviewFixturePatternRequest>{hold.getError().value()};
    if (!stopAfter.isSuccess())
        return Result<PreviewFixturePatternRequest>{stopAfter.getError().value()};
    return Result<PreviewFixturePatternRequest>{{values.getValue().value(), fadeIn.getValue().value().value_or(0),
                                                 fadeOut.getValue().value().value_or(0),
                                                 hold.getValue().value().value_or(0), stopAfter.getValue().value()}};
}

inline Result<SetFixtureLiveRequest> setFixtureLiveRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "fixture live request", {"values", "timeout_ms"});
    if (!fields.isSuccess())
        return Result<SetFixtureLiveRequest>{fields.getError().value()};
    auto values = fixtureChannelValuesFromJson(json, "fixture live request");
    auto timeout = json_codec::requiredUnsigned<uint32_t>(json, "fixture live request", "timeout_ms",
                                                          MAX_FIXTURE_CONTROL_DURATION_MS);
    if (!values.isSuccess())
        return Result<SetFixtureLiveRequest>{values.getError().value()};
    if (!timeout.isSuccess())
        return Result<SetFixtureLiveRequest>{timeout.getError().value()};
    if (timeout.getValue().value() == 0)
        return json_codec::invalid<SetFixtureLiveRequest>("fixture live request.timeout_ms must be greater than 0");
    return Result<SetFixtureLiveRequest>{{values.getValue().value(), timeout.getValue().value()}};
}

} // namespace creatures::api
