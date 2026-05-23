
#include <gtest/gtest.h>

#include "model/DmxFixture.h"
#include "server/fixture/FixturePatternRunner.h"

namespace creatures {

namespace {

DmxFixture buildTwoChannelFixture() {
    DmxFixture f;
    f.id = "11111111-2222-4333-8444-555555555555";
    f.name = "Test Light";
    f.type = FixtureType::Light;
    f.channel_offset = 100;
    FixtureChannel red;
    red.offset = 0;
    red.name = "red";
    red.kind = "color_red";
    FixtureChannel green;
    green.offset = 1;
    green.name = "green";
    green.kind = "color_green";
    f.channels = {red, green};
    return f;
}

} // namespace

TEST(FixturePatternRunnerSetLiveTest, SuccessOnValidInput) {
    FixturePatternRunner runner;
    const auto fixture = buildTwoChannelFixture();
    std::vector<std::pair<std::string, uint8_t>> values{{"red", 255}, {"green", 128}};

    EXPECT_FALSE(runner.hasLive(fixture.id));
    EXPECT_TRUE(runner.setLive(fixture, values, /*timeoutMs=*/5000, /*universe=*/1, /*currentFrame=*/0));
    EXPECT_TRUE(runner.hasLive(fixture.id));
}

TEST(FixturePatternRunnerSetLiveTest, RejectsEmptyValues) {
    FixturePatternRunner runner;
    const auto fixture = buildTwoChannelFixture();
    std::vector<std::pair<std::string, uint8_t>> values{};

    EXPECT_FALSE(runner.setLive(fixture, values, 5000, 1, 0));
    EXPECT_FALSE(runner.hasLive(fixture.id));
}

TEST(FixturePatternRunnerSetLiveTest, RejectsZeroTimeout) {
    FixturePatternRunner runner;
    const auto fixture = buildTwoChannelFixture();
    std::vector<std::pair<std::string, uint8_t>> values{{"red", 255}};

    EXPECT_FALSE(runner.setLive(fixture, values, /*timeoutMs=*/0, 1, 0));
    EXPECT_FALSE(runner.hasLive(fixture.id));
}

TEST(FixturePatternRunnerSetLiveTest, RejectsUnknownChannelName) {
    FixturePatternRunner runner;
    const auto fixture = buildTwoChannelFixture();
    std::vector<std::pair<std::string, uint8_t>> values{{"blue", 200}}; // not on fixture

    EXPECT_FALSE(runner.setLive(fixture, values, 5000, 1, 0));
    // Unknown channel must be a hard failure with no state change — verifies the rollback.
    EXPECT_FALSE(runner.hasLive(fixture.id));
}

TEST(FixturePatternRunnerSetLiveTest, PartialFailureRollsBackPreviousValues) {
    FixturePatternRunner runner;
    const auto fixture = buildTwoChannelFixture();
    // First call sets both channels successfully.
    std::vector<std::pair<std::string, uint8_t>> first{{"red", 255}, {"green", 128}};
    ASSERT_TRUE(runner.setLive(fixture, first, 5000, 1, 0));
    ASSERT_TRUE(runner.hasLive(fixture.id));

    // Second call has a valid + invalid pair. The whole call must fail and the
    // previous live state must remain intact — no partial writes.
    std::vector<std::pair<std::string, uint8_t>> second{{"red", 10}, {"bogus", 99}};
    EXPECT_FALSE(runner.setLive(fixture, second, 5000, 1, 0));
    // hasLive still true because the entry from the first call wasn't removed.
    EXPECT_TRUE(runner.hasLive(fixture.id));
}

TEST(FixturePatternRunnerSetLiveTest, SubsequentCallExtendsExistingSession) {
    FixturePatternRunner runner;
    const auto fixture = buildTwoChannelFixture();
    ASSERT_TRUE(runner.setLive(fixture, {{"red", 50}}, 5000, 1, /*currentFrame=*/0));
    ASSERT_TRUE(runner.hasLive(fixture.id));

    // Second call should succeed and hold the green channel from before (still 0 here)
    // while updating red. No re-validation of timeout aside from > 0.
    EXPECT_TRUE(runner.setLive(fixture, {{"red", 200}}, /*timeoutMs=*/100, 1, /*currentFrame=*/1000));
    EXPECT_TRUE(runner.hasLive(fixture.id));
}

TEST(FixturePatternRunnerSetLiveTest, EmptyChannelsFixtureFails) {
    FixturePatternRunner runner;
    DmxFixture fixture;
    fixture.id = "deadbeef-0000-4000-8000-000000000000";
    fixture.name = "No Channels";
    fixture.type = FixtureType::Generic;
    fixture.channel_offset = 0;
    // channels deliberately empty

    std::vector<std::pair<std::string, uint8_t>> values{{"red", 255}};
    EXPECT_FALSE(runner.setLive(fixture, values, 5000, 1, 0));
}

} // namespace creatures
