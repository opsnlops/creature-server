
#include <gtest/gtest.h>

#include "server/fixture/FixturePatternRunner.h"

namespace creatures {

TEST(FixturePatternRunnerLerpTest, ReturnsStartAtTZero) {
    EXPECT_EQ(FixturePatternRunner::lerp(0, 255, 0.0), 0);
    EXPECT_EQ(FixturePatternRunner::lerp(100, 50, 0.0), 100);
    EXPECT_EQ(FixturePatternRunner::lerp(200, 10, 0.0), 200);
}

TEST(FixturePatternRunnerLerpTest, ReturnsTargetAtTOne) {
    EXPECT_EQ(FixturePatternRunner::lerp(0, 255, 1.0), 255);
    EXPECT_EQ(FixturePatternRunner::lerp(100, 50, 1.0), 50);
    EXPECT_EQ(FixturePatternRunner::lerp(200, 10, 1.0), 10);
}

TEST(FixturePatternRunnerLerpTest, ClampsTBelowZero) { EXPECT_EQ(FixturePatternRunner::lerp(100, 200, -0.5), 100); }

TEST(FixturePatternRunnerLerpTest, ClampsTAboveOne) { EXPECT_EQ(FixturePatternRunner::lerp(100, 200, 1.5), 200); }

TEST(FixturePatternRunnerLerpTest, MidpointIsArithmeticMeanWithinPlusMinusOne) {
    // 0 -> 255 at t=0.5 = 127.5, should round to 127 or 128.
    const auto mid = FixturePatternRunner::lerp(0, 255, 0.5);
    EXPECT_TRUE(mid == 127 || mid == 128) << "lerp midpoint was " << static_cast<int>(mid);

    // 100 -> 50 at t=0.5 = 75 exactly.
    EXPECT_EQ(FixturePatternRunner::lerp(100, 50, 0.5), 75);

    // 10 -> 250 at t=0.25 = 70.
    EXPECT_EQ(FixturePatternRunner::lerp(10, 250, 0.25), 70);
}

TEST(FixturePatternRunnerLerpTest, FullRangeBoundsAreSafe) {
    // No over/underflow at extremes.
    EXPECT_EQ(FixturePatternRunner::lerp(0, 0, 0.5), 0);
    EXPECT_EQ(FixturePatternRunner::lerp(255, 255, 0.5), 255);
    EXPECT_EQ(FixturePatternRunner::lerp(255, 0, 0.5), 128); // rounds up from 127.5
}

// Integration-ish: a pattern with hold_ms=0 should never naturally advance into FadeOut.
// We can't run tick() without an event loop, but we can confirm the active-pattern map stays
// non-empty across many ticks. This is sketched as a TODO since exercising tick() needs
// either a mock event loop or test-only constructor; the lerp tests above are the load-bearing
// math validations.

} // namespace creatures
