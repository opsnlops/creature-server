
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/Track.h"
#include "server/database.h"

namespace creatures {

namespace {

nlohmann::json makeTrackJson() {
    return nlohmann::json{
        {"id", "track-1"},
        {"animation_id", "anim-1"},
        {"frames", nlohmann::json::array({"abc=", "def="})},
    };
}

} // namespace

TEST(TrackDualIdTest, AcceptsCreatureIdOnly) {
    auto j = makeTrackJson();
    j["creature_id"] = "creature-1";
    auto result = Database::parseTrackJson(j);
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");
    const auto track = result.getValue().value();
    EXPECT_EQ(track.creature_id, "creature-1");
    EXPECT_TRUE(track.fixture_id.empty());
}

TEST(TrackDualIdTest, AcceptsFixtureIdOnly) {
    auto j = makeTrackJson();
    j["fixture_id"] = "fixture-1";
    auto result = Database::parseTrackJson(j);
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");
    const auto track = result.getValue().value();
    EXPECT_EQ(track.fixture_id, "fixture-1");
    EXPECT_TRUE(track.creature_id.empty());
}

TEST(TrackDualIdTest, RejectsNeitherSet) {
    auto j = makeTrackJson();
    // Neither creature_id nor fixture_id present.
    auto result = Database::parseTrackJson(j);
    EXPECT_FALSE(result.isSuccess());
}

TEST(TrackDualIdTest, RejectsBothSet) {
    auto j = makeTrackJson();
    j["creature_id"] = "creature-1";
    j["fixture_id"] = "fixture-1";
    auto result = Database::parseTrackJson(j);
    EXPECT_FALSE(result.isSuccess());
}

TEST(TrackDualIdTest, RejectsBothPresentButEmpty) {
    // Defensive: both fields explicitly present but empty strings — same as neither.
    auto j = makeTrackJson();
    j["creature_id"] = "";
    j["fixture_id"] = "";
    auto result = Database::parseTrackJson(j);
    EXPECT_FALSE(result.isSuccess());
}

} // namespace creatures
