
#include <set>
#include <string>

#include <spdlog/spdlog.h>

#include "exception/exception.h"
#include "model/DmxFixture.h"
#include "server/database.h"
#include "util/ObservabilityManager.h"
#include "util/helpers.h"

namespace creatures {

extern std::vector<std::string> fixture_required_top_level_fields;
extern std::vector<std::string> fixture_required_channel_fields;
extern std::vector<std::string> fixture_required_pattern_fields;
extern std::vector<std::string> fixture_required_pattern_value_fields;
extern std::vector<std::string> fixture_required_binding_fields;

extern std::shared_ptr<ObservabilityManager> observability;

namespace {

// Hard caps on fixture array sizes. Picked to be comfortably larger than any plausible
// real fixture (a single device with hundreds of channels or thousands of patterns is not
// a thing that exists) but small enough to neutralize JSON-bomb DoS attempts. A single
// 16 MB Mongo document can pack a lot of bindings; without these the validate endpoint
// is an N-query amplifier (see security review H2).
constexpr size_t MAX_CHANNELS_PER_FIXTURE = 64;
constexpr size_t MAX_PATTERNS_PER_FIXTURE = 256;
constexpr size_t MAX_VALUES_PER_PATTERN = 64;
constexpr size_t MAX_BINDINGS_PER_FIXTURE = 256;


template <typename T> Result<T> invalidData(const std::shared_ptr<OperationSpan> &span, const std::string &message) {
    warn(message);
    if (span) {
        span->setError(message);
        span->setAttribute("error.type", "InvalidData");
        span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
    }
    return Result<T>{ServerError(ServerError::InvalidData, message)};
}

} // namespace

Result<creatures::DmxFixture> Database::fixtureFromJson(json fixtureJson, std::shared_ptr<OperationSpan> parentSpan) {

    if (!parentSpan) {
        warn("no parent span provided for Database.fixtureFromJson, creating a root span");
    }

    auto span = creatures::observability->createChildOperationSpan("Database.fixtureFromJson", parentSpan);

    debug("attempting to create a DmxFixture from JSON");

    try {
        DmxFixture fixture;

        // id
        if (!fixtureJson.contains("id") || fixtureJson["id"].is_null() || !fixtureJson["id"].is_string()) {
            return invalidData<DmxFixture>(span, "Missing or invalid field 'id' in fixture JSON");
        }
        fixture.id = fixtureJson["id"].get<std::string>();
        if (fixture.id.empty()) {
            return invalidData<DmxFixture>(span, "Fixture 'id' is empty");
        }

        // name
        if (!fixtureJson.contains("name") || fixtureJson["name"].is_null() || !fixtureJson["name"].is_string()) {
            return invalidData<DmxFixture>(span, "Missing or invalid field 'name' in fixture JSON");
        }
        fixture.name = fixtureJson["name"].get<std::string>();
        if (fixture.name.empty()) {
            return invalidData<DmxFixture>(span, "Fixture 'name' is empty");
        }

        // type
        if (!fixtureJson.contains("type") || fixtureJson["type"].is_null() || !fixtureJson["type"].is_string()) {
            return invalidData<DmxFixture>(span, "Missing or invalid field 'type' in fixture JSON");
        }
        fixture.type = fixtureTypeFromString(fixtureJson["type"].get<std::string>());

        // channel_offset
        if (!fixtureJson.contains("channel_offset") || fixtureJson["channel_offset"].is_null()) {
            return invalidData<DmxFixture>(span, "Missing or null field 'channel_offset' in fixture JSON");
        }
        const auto channelOffset = fixtureJson["channel_offset"].get<int64_t>();
        if (channelOffset < 0 || channelOffset > 511) {
            return invalidData<DmxFixture>(
                span, fmt::format("Fixture 'channel_offset' must be in [0, 511]; got {}", channelOffset));
        }
        fixture.channel_offset = static_cast<uint16_t>(channelOffset);

        // assigned_universe (optional, nullable). E1.31 universes are valid in [1, 63999];
        // 0 is reserved and values above 63999 are out of spec (security review L1).
        if (fixtureJson.contains("assigned_universe") && !fixtureJson["assigned_universe"].is_null()) {
            const auto rawUniverse = fixtureJson["assigned_universe"].get<int64_t>();
            if (rawUniverse < 1 || rawUniverse > 63999) {
                return invalidData<DmxFixture>(
                    span, fmt::format("Fixture 'assigned_universe' must be in [1, 63999]; got {}", rawUniverse));
            }
            fixture.assigned_universe = static_cast<universe_t>(rawUniverse);
        }

        // channels (required, non-empty, bounded)
        if (!fixtureJson.contains("channels") || !fixtureJson["channels"].is_array()) {
            return invalidData<DmxFixture>(span, "Missing or non-array field 'channels' in fixture JSON");
        }
        if (fixtureJson["channels"].empty()) {
            return invalidData<DmxFixture>(span, "Fixture 'channels' must be non-empty");
        }
        if (fixtureJson["channels"].size() > MAX_CHANNELS_PER_FIXTURE) {
            return invalidData<DmxFixture>(
                span, fmt::format("Fixture 'channels' must have at most {} entries; got {}",
                                  MAX_CHANNELS_PER_FIXTURE, fixtureJson["channels"].size()));
        }

        std::set<std::string> seenChannelNames;
        uint16_t maxOffset = 0;
        for (const auto &channelJson : fixtureJson["channels"]) {
            if (!channelJson.contains("offset") || !channelJson["offset"].is_number()) {
                return invalidData<DmxFixture>(span, "Channel missing required field 'offset' (uint16)");
            }
            if (!channelJson.contains("name") || !channelJson["name"].is_string()) {
                return invalidData<DmxFixture>(span, "Channel missing required field 'name' (string)");
            }

            FixtureChannel ch;
            const auto rawOffset = channelJson["offset"].get<int64_t>();
            if (rawOffset < 0 || rawOffset > 511) {
                return invalidData<DmxFixture>(span,
                                               fmt::format("Channel 'offset' must be in [0, 511]; got {}", rawOffset));
            }
            ch.offset = static_cast<uint16_t>(rawOffset);
            ch.name = channelJson["name"].get<std::string>();
            if (ch.name.empty()) {
                return invalidData<DmxFixture>(span, "Channel 'name' is empty");
            }
            if (!seenChannelNames.insert(ch.name).second) {
                return invalidData<DmxFixture>(
                    span, fmt::format("Duplicate channel name '{}' on fixture {}", ch.name, fixture.id));
            }
            ch.kind = "generic";
            if (channelJson.contains("kind") && !channelJson["kind"].is_null()) {
                if (!channelJson["kind"].is_string()) {
                    return invalidData<DmxFixture>(span, "Channel 'kind' must be a string");
                }
                ch.kind = channelJson["kind"].get<std::string>();
            }
            maxOffset = std::max(maxOffset, ch.offset);
            fixture.channels.push_back(ch);
        }

        // Universe fit check: channel_offset + max(channel.offset) must be <= 511
        if (static_cast<uint32_t>(fixture.channel_offset) + static_cast<uint32_t>(maxOffset) > 511) {
            return invalidData<DmxFixture>(
                span, fmt::format("Fixture {} does not fit in a universe: channel_offset {} + max channel offset {} > "
                                  "511",
                                  fixture.id, fixture.channel_offset, maxOffset));
        }

        // patterns (optional, bounded)
        std::set<std::string> seenPatternIds;
        if (fixtureJson.contains("patterns") && !fixtureJson["patterns"].is_null()) {
            if (!fixtureJson["patterns"].is_array()) {
                return invalidData<DmxFixture>(span, "Fixture 'patterns' must be an array");
            }
            if (fixtureJson["patterns"].size() > MAX_PATTERNS_PER_FIXTURE) {
                return invalidData<DmxFixture>(
                    span, fmt::format("Fixture 'patterns' must have at most {} entries; got {}",
                                      MAX_PATTERNS_PER_FIXTURE, fixtureJson["patterns"].size()));
            }
            for (const auto &patternJson : fixtureJson["patterns"]) {
                FixturePattern p;
                if (!patternJson.contains("id") || !patternJson["id"].is_string()) {
                    return invalidData<DmxFixture>(span, "Pattern missing required field 'id'");
                }
                p.id = patternJson["id"].get<std::string>();
                if (p.id.empty()) {
                    return invalidData<DmxFixture>(span, "Pattern 'id' is empty");
                }
                if (!seenPatternIds.insert(p.id).second) {
                    return invalidData<DmxFixture>(
                        span, fmt::format("Duplicate pattern id '{}' on fixture {}", p.id, fixture.id));
                }
                if (!patternJson.contains("name") || !patternJson["name"].is_string()) {
                    return invalidData<DmxFixture>(span, "Pattern missing required field 'name'");
                }
                p.name = patternJson["name"].get<std::string>();
                p.fade_in_ms = patternJson.value("fade_in_ms", 0u);
                p.fade_out_ms = patternJson.value("fade_out_ms", 0u);
                p.hold_ms = patternJson.value("hold_ms", 0u);

                if (!patternJson.contains("values") || !patternJson["values"].is_array()) {
                    return invalidData<DmxFixture>(span,
                                                   fmt::format("Pattern '{}' missing required 'values' array", p.id));
                }
                if (patternJson["values"].size() > MAX_VALUES_PER_PATTERN) {
                    return invalidData<DmxFixture>(
                        span, fmt::format("Pattern '{}' has {} values, exceeds max {}", p.id,
                                          patternJson["values"].size(), MAX_VALUES_PER_PATTERN));
                }
                for (const auto &valueJson : patternJson["values"]) {
                    if (!valueJson.contains("channel") || !valueJson["channel"].is_string()) {
                        return invalidData<DmxFixture>(span, "Pattern value missing 'channel' (string)");
                    }
                    if (!valueJson.contains("value") || !valueJson["value"].is_number()) {
                        return invalidData<DmxFixture>(span, "Pattern value missing 'value' (uint8)");
                    }
                    FixturePatternValue v;
                    v.channel = valueJson["channel"].get<std::string>();
                    const auto rawValue = valueJson["value"].get<int64_t>();
                    if (rawValue < 0 || rawValue > 255) {
                        return invalidData<DmxFixture>(
                            span, fmt::format("Pattern value must be in [0, 255]; got {}", rawValue));
                    }
                    v.value = static_cast<uint8_t>(rawValue);
                    if (!seenChannelNames.count(v.channel)) {
                        return invalidData<DmxFixture>(
                            span, fmt::format("Pattern '{}' references unknown channel '{}'", p.id, v.channel));
                    }
                    p.values.push_back(v);
                }
                fixture.patterns.push_back(p);
            }
        }

        // bindings (optional, bounded)
        if (fixtureJson.contains("bindings") && !fixtureJson["bindings"].is_null()) {
            if (!fixtureJson["bindings"].is_array()) {
                return invalidData<DmxFixture>(span, "Fixture 'bindings' must be an array");
            }
            if (fixtureJson["bindings"].size() > MAX_BINDINGS_PER_FIXTURE) {
                return invalidData<DmxFixture>(
                    span, fmt::format("Fixture 'bindings' must have at most {} entries; got {}",
                                      MAX_BINDINGS_PER_FIXTURE, fixtureJson["bindings"].size()));
            }
            static const std::set<std::string> validReasons = {"play",     "playlist",  "ad_hoc",   "idle",
                                                               "disabled", "cancelled", "streaming"};
            static const std::set<std::string> validStates = {"running", "idle", "disabled", "stopped"};

            for (const auto &bindingJson : fixtureJson["bindings"]) {
                FixtureBinding b;
                if (!bindingJson.contains("creature_id") || !bindingJson["creature_id"].is_string()) {
                    return invalidData<DmxFixture>(span, "Binding missing required field 'creature_id'");
                }
                b.creature_id = bindingJson["creature_id"].get<std::string>();
                if (b.creature_id.empty()) {
                    return invalidData<DmxFixture>(span, "Binding 'creature_id' is empty");
                }
                if (!bindingJson.contains("pattern_id") || !bindingJson["pattern_id"].is_string()) {
                    return invalidData<DmxFixture>(span, "Binding missing required field 'pattern_id'");
                }
                b.pattern_id = bindingJson["pattern_id"].get<std::string>();
                if (!seenPatternIds.count(b.pattern_id)) {
                    return invalidData<DmxFixture>(
                        span, fmt::format("Binding references unknown pattern_id '{}' on fixture {}", b.pattern_id,
                                          fixture.id));
                }
                if (bindingJson.contains("on_reason") && !bindingJson["on_reason"].is_null()) {
                    if (!bindingJson["on_reason"].is_string()) {
                        return invalidData<DmxFixture>(span, "Binding 'on_reason' must be a string or null");
                    }
                    const auto reason = bindingJson["on_reason"].get<std::string>();
                    if (!validReasons.count(reason)) {
                        return invalidData<DmxFixture>(
                            span, fmt::format("Binding 'on_reason' '{}' is not a known activity reason", reason));
                    }
                    b.on_reason = reason;
                }
                if (bindingJson.contains("on_state") && !bindingJson["on_state"].is_null()) {
                    if (!bindingJson["on_state"].is_string()) {
                        return invalidData<DmxFixture>(span, "Binding 'on_state' must be a string or null");
                    }
                    const auto state = bindingJson["on_state"].get<std::string>();
                    if (!validStates.count(state)) {
                        return invalidData<DmxFixture>(
                            span, fmt::format("Binding 'on_state' '{}' is not a known activity state", state));
                    }
                    b.on_state = state;
                }
                fixture.bindings.push_back(b);
            }
        }

        if (span) {
            span->setSuccess();
            span->setAttribute("fixture.id", fixture.id);
            span->setAttribute("fixture.name", fixture.name);
            span->setAttribute("fixture.type", fixtureTypeToString(fixture.type));
            span->setAttribute("fixture.channels_count", static_cast<int64_t>(fixture.channels.size()));
            span->setAttribute("fixture.patterns_count", static_cast<int64_t>(fixture.patterns.size()));
            span->setAttribute("fixture.bindings_count", static_cast<int64_t>(fixture.bindings.size()));
        }
        debug("✅ Successfully created fixture from JSON: id='{}', name='{}', channels={}, patterns={}, bindings={}",
              fixture.id, fixture.name, fixture.channels.size(), fixture.patterns.size(), fixture.bindings.size());
        return Result<DmxFixture>{fixture};

    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage = fmt::format("Error while converting JSON to DmxFixture: {}", e.what());
        warn(errorMessage);
        if (span) {
            span->recordException(e);
            span->setAttribute("error.type", "JsonParsingException");
            span->setAttribute("error.message", e.what());
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
        return Result<DmxFixture>{ServerError(ServerError::InvalidData, errorMessage)};
    }
}

Result<creatures::DmxFixture> Database::parseFixtureJson(json fixtureJson, std::shared_ptr<OperationSpan> parentSpan) {
    return fixtureFromJson(std::move(fixtureJson), std::move(parentSpan));
}

Result<bool> Database::validateFixtureJson(const nlohmann::json &json) {

    auto topOkay = has_required_fields(json, fixture_required_top_level_fields);
    if (!topOkay.isSuccess()) {
        return topOkay;
    }

    if (json.contains("channels") && json["channels"].is_array()) {
        for (const auto &ch : json["channels"]) {
            auto chOkay = has_required_fields(ch, fixture_required_channel_fields);
            if (!chOkay.isSuccess()) {
                return chOkay;
            }
        }
    }

    if (json.contains("patterns") && json["patterns"].is_array()) {
        for (const auto &p : json["patterns"]) {
            auto pOkay = has_required_fields(p, fixture_required_pattern_fields);
            if (!pOkay.isSuccess()) {
                return pOkay;
            }
            if (p.contains("values") && p["values"].is_array()) {
                for (const auto &v : p["values"]) {
                    auto vOkay = has_required_fields(v, fixture_required_pattern_value_fields);
                    if (!vOkay.isSuccess()) {
                        return vOkay;
                    }
                }
            }
        }
    }

    if (json.contains("bindings") && json["bindings"].is_array()) {
        for (const auto &b : json["bindings"]) {
            auto bOkay = has_required_fields(b, fixture_required_binding_fields);
            if (!bOkay.isSuccess()) {
                return bOkay;
            }
        }
    }

    return Result<bool>{true};
}

} // namespace creatures
