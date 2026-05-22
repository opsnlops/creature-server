
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/DmxFixture.h"
#include "server/database.h"

namespace creatures {

namespace {

nlohmann::json makeValidFixtureJson() {
    return nlohmann::json{
        {"id", "8e3a4b5c-1d2f-4e6a-9b0c-7f8e9d0a1b2c"},
        {"name", "Stage Left Spot"},
        {"type", "light"},
        {"channel_offset", 500},
        {"assigned_universe", 1},
        {"channels", nlohmann::json::array({
                         {{"offset", 0}, {"name", "red"}, {"kind", "color_red"}},
                         {{"offset", 1}, {"name", "green"}, {"kind", "color_green"}},
                         {{"offset", 2}, {"name", "brightness"}, {"kind", "master_dimmer"}},
                     })},
        {"patterns", nlohmann::json::array({
                         {{"id", "7d2a3b4c-5e6f-4789-a0b1-c2d3e4f5a6b7"},
                          {"name", "Red Glow"},
                          {"values", nlohmann::json::array({{{"channel", "red"}, {"value", 255}},
                                                            {{"channel", "brightness"}, {"value", 200}}})},
                          {"fade_in_ms", 250},
                          {"fade_out_ms", 500},
                          {"hold_ms", 0}},
                     })},
        {"bindings", nlohmann::json::array({
                         {{"creature_id", "1a2b3c4d-5e6f-4789-a0b1-c2d3e4f5a6b7"},
                          {"on_reason", "ad_hoc"},
                          {"on_state", "running"},
                          {"pattern_id", "7d2a3b4c-5e6f-4789-a0b1-c2d3e4f5a6b7"}},
                     })},
    };
}

} // namespace

TEST(DmxFixtureJsonTest, RoundTripsValidConfig) {
    auto j = makeValidFixtureJson();
    auto result = Database::parseFixtureJson(j);
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");
    const auto fixture = result.getValue().value();
    EXPECT_EQ(fixture.id, "8e3a4b5c-1d2f-4e6a-9b0c-7f8e9d0a1b2c");
    EXPECT_EQ(fixture.name, "Stage Left Spot");
    EXPECT_EQ(fixture.type, FixtureType::Light);
    EXPECT_EQ(fixture.channel_offset, 500);
    ASSERT_TRUE(fixture.assigned_universe.has_value());
    EXPECT_EQ(*fixture.assigned_universe, 1u);
    ASSERT_EQ(fixture.channels.size(), 3u);
    EXPECT_EQ(fixture.channels[0].name, "red");
    EXPECT_EQ(fixture.channels[0].kind, "color_red");
    ASSERT_EQ(fixture.patterns.size(), 1u);
    EXPECT_EQ(fixture.patterns[0].name, "Red Glow");
    EXPECT_EQ(fixture.patterns[0].fade_in_ms, 250u);
    EXPECT_EQ(fixture.patterns[0].fade_out_ms, 500u);
    EXPECT_EQ(fixture.patterns[0].hold_ms, 0u);
    ASSERT_EQ(fixture.bindings.size(), 1u);
    EXPECT_EQ(fixture.bindings[0].creature_id, "1a2b3c4d-5e6f-4789-a0b1-c2d3e4f5a6b7");
    ASSERT_TRUE(fixture.bindings[0].on_reason.has_value());
    EXPECT_EQ(*fixture.bindings[0].on_reason, "ad_hoc");
    ASSERT_TRUE(fixture.bindings[0].on_state.has_value());
    EXPECT_EQ(*fixture.bindings[0].on_state, "running");
}

TEST(DmxFixtureJsonTest, AcceptsNullAssignedUniverse) {
    auto j = makeValidFixtureJson();
    j["assigned_universe"] = nullptr;
    auto result = Database::parseFixtureJson(j);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_FALSE(result.getValue().value().assigned_universe.has_value());
}

TEST(DmxFixtureJsonTest, AcceptsMissingAssignedUniverse) {
    auto j = makeValidFixtureJson();
    j.erase("assigned_universe");
    auto result = Database::parseFixtureJson(j);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_FALSE(result.getValue().value().assigned_universe.has_value());
}

TEST(DmxFixtureJsonTest, RejectsMissingId) {
    auto j = makeValidFixtureJson();
    j.erase("id");
    auto result = Database::parseFixtureJson(j);
    EXPECT_FALSE(result.isSuccess());
}

TEST(DmxFixtureJsonTest, RejectsEmptyChannels) {
    auto j = makeValidFixtureJson();
    j["channels"] = nlohmann::json::array();
    auto result = Database::parseFixtureJson(j);
    EXPECT_FALSE(result.isSuccess());
}

TEST(DmxFixtureJsonTest, RejectsFixtureThatOverrunsUniverse) {
    auto j = makeValidFixtureJson();
    // channel_offset 500 + max(channel.offset) must be <= 511. Push it past.
    j["channels"].push_back({{"offset", 12}, {"name", "overshoot"}});
    auto result = Database::parseFixtureJson(j);
    EXPECT_FALSE(result.isSuccess());
}

TEST(DmxFixtureJsonTest, RejectsDuplicateChannelNames) {
    auto j = makeValidFixtureJson();
    j["channels"].push_back({{"offset", 3}, {"name", "red"}});
    auto result = Database::parseFixtureJson(j);
    EXPECT_FALSE(result.isSuccess());
}

TEST(DmxFixtureJsonTest, RejectsPatternReferencingUnknownChannel) {
    auto j = makeValidFixtureJson();
    j["patterns"][0]["values"].push_back({{"channel", "no_such_channel"}, {"value", 100}});
    auto result = Database::parseFixtureJson(j);
    EXPECT_FALSE(result.isSuccess());
}

TEST(DmxFixtureJsonTest, RejectsBindingReferencingUnknownPattern) {
    auto j = makeValidFixtureJson();
    j["bindings"][0]["pattern_id"] = "deadbeef-0000-0000-0000-000000000000";
    auto result = Database::parseFixtureJson(j);
    EXPECT_FALSE(result.isSuccess());
}

TEST(DmxFixtureJsonTest, RejectsBindingWithUnknownReasonEnum) {
    auto j = makeValidFixtureJson();
    j["bindings"][0]["on_reason"] = "definitely_not_a_real_reason";
    auto result = Database::parseFixtureJson(j);
    EXPECT_FALSE(result.isSuccess());
}

TEST(DmxFixtureJsonTest, AcceptsBindingWithWildcardFilters) {
    auto j = makeValidFixtureJson();
    j["bindings"][0].erase("on_reason");
    j["bindings"][0].erase("on_state");
    auto result = Database::parseFixtureJson(j);
    ASSERT_TRUE(result.isSuccess());
    const auto fixture = result.getValue().value();
    EXPECT_FALSE(fixture.bindings[0].on_reason.has_value());
    EXPECT_FALSE(fixture.bindings[0].on_state.has_value());
}

TEST(DmxFixtureJsonTest, UnknownTypeFallsBackToGeneric) {
    auto j = makeValidFixtureJson();
    j["type"] = "future_alien_device_type";
    auto result = Database::parseFixtureJson(j);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.getValue().value().type, FixtureType::Generic);
}

TEST(DmxFixtureJsonTest, RoundTripsThroughDto) {
    auto j = makeValidFixtureJson();
    auto fixture = Database::parseFixtureJson(j).getValue().value();
    auto dto = convertToDto(fixture);
    auto roundTripped = convertFromDto(dto.getPtr());
    EXPECT_EQ(roundTripped.id, fixture.id);
    EXPECT_EQ(roundTripped.name, fixture.name);
    EXPECT_EQ(roundTripped.type, fixture.type);
    EXPECT_EQ(roundTripped.channel_offset, fixture.channel_offset);
    EXPECT_EQ(roundTripped.channels.size(), fixture.channels.size());
    EXPECT_EQ(roundTripped.patterns.size(), fixture.patterns.size());
    EXPECT_EQ(roundTripped.bindings.size(), fixture.bindings.size());
    ASSERT_EQ(roundTripped.bindings.size(), 1u);
    EXPECT_EQ(roundTripped.bindings[0].creature_id, fixture.bindings[0].creature_id);
    EXPECT_EQ(roundTripped.bindings[0].on_reason, fixture.bindings[0].on_reason);
}

TEST(DmxFixtureJsonTest, ValidateFixtureJsonCatchesMissingTopLevelField) {
    auto j = makeValidFixtureJson();
    j.erase("type");
    auto result = Database::validateFixtureJson(j);
    EXPECT_FALSE(result.isSuccess());
}

} // namespace creatures
