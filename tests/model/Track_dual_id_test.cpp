
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/Track.h"

namespace creatures {

namespace {

nlohmann::json makeTrackJson() {
    return nlohmann::json{
        {"id", "aaaaaaaa-1111-4222-8333-444455556666"},
        {"animation_id", "eeeeeeee-1111-4222-8333-444455556666"},
        {"frames", nlohmann::json::array({"abc=", "def="})},
    };
}

} // namespace

TEST(TrackDualIdTest, AcceptsCreatureIdOnly) {
    auto j = makeTrackJson();
    j["creature_id"] = "bbbbbbbb-1111-4222-8333-444455556666";
    auto result = trackFromJson(j);
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");
    const auto track = result.getValue().value();
    EXPECT_EQ(track.creature_id, "bbbbbbbb-1111-4222-8333-444455556666");
    EXPECT_TRUE(track.fixture_id.empty());
}

TEST(TrackDualIdTest, AcceptsFixtureIdOnly) {
    auto j = makeTrackJson();
    j["fixture_id"] = "cccccccc-1111-4222-8333-444455556666";
    auto result = trackFromJson(j);
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");
    const auto track = result.getValue().value();
    EXPECT_EQ(track.fixture_id, "cccccccc-1111-4222-8333-444455556666");
    EXPECT_TRUE(track.creature_id.empty());
}

TEST(TrackDualIdTest, RejectsNeitherSet) {
    auto j = makeTrackJson();
    // Neither creature_id nor fixture_id present.
    auto result = trackFromJson(j);
    EXPECT_FALSE(result.isSuccess());
}

TEST(TrackDualIdTest, RejectsBothSet) {
    auto j = makeTrackJson();
    j["creature_id"] = "bbbbbbbb-1111-4222-8333-444455556666";
    j["fixture_id"] = "cccccccc-1111-4222-8333-444455556666";
    auto result = trackFromJson(j);
    EXPECT_FALSE(result.isSuccess());
}

TEST(TrackDualIdTest, RejectsCreatureIdWithNullFixtureId) {
    auto j = makeTrackJson();
    j["creature_id"] = "bbbbbbbb-1111-4222-8333-444455556666";
    j["fixture_id"] = nullptr;
    EXPECT_FALSE(trackFromJson(j).isSuccess());
}

TEST(TrackDualIdTest, RejectsFixtureIdWithNullCreatureId) {
    auto j = makeTrackJson();
    j["creature_id"] = nullptr;
    j["fixture_id"] = "cccccccc-1111-4222-8333-444455556666";
    EXPECT_FALSE(trackFromJson(j).isSuccess());
}

TEST(TrackDualIdTest, RejectsBothNull) {
    auto j = makeTrackJson();
    j["creature_id"] = nullptr;
    j["fixture_id"] = nullptr;
    auto result = trackFromJson(j);
    EXPECT_FALSE(result.isSuccess());
}

TEST(TrackDualIdTest, RejectsBothPresentButEmpty) {
    // Defensive: both fields explicitly present but empty strings — same as neither.
    auto j = makeTrackJson();
    j["creature_id"] = "";
    j["fixture_id"] = "";
    auto result = trackFromJson(j);
    EXPECT_FALSE(result.isSuccess());
}

} // namespace creatures
