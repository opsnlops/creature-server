#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/IxmlTimedTokens.h"

using creatures::voice::packTimedTokens;
using creatures::voice::parseTimedTokens;
using creatures::voice::TimedToken;

TEST(IxmlTimedTokens, PacksToTheCompactForm) {
    const std::vector<TimedToken> tokens = {{0.0, 0.25, "A"}, {0.25, 0.5, "B"}};
    EXPECT_EQ(packTimedTokens(tokens), "0.000 0.250 A;0.250 0.500 B");
}

TEST(IxmlTimedTokens, RoundTripsShapesAndWords) {
    const std::vector<TimedToken> tokens = {{0.0, 0.3, "Hello,"}, {0.3, 0.75, "awake?"}};
    const auto parsed = parseTimedTokens(packTimedTokens(tokens));
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_DOUBLE_EQ(parsed[0].start, 0.0);
    EXPECT_DOUBLE_EQ(parsed[0].end, 0.3);
    EXPECT_EQ(parsed[0].token, "Hello,");
    EXPECT_EQ(parsed[1].token, "awake?");
    EXPECT_DOUBLE_EQ(parsed[1].end, 0.75);
}

TEST(IxmlTimedTokens, SanitizesTheSeparatorInsideAToken) {
    // A ';' in a token would corrupt the packing, so it's replaced with ',' — the
    // entry count stays correct rather than splitting into a bogus extra entry.
    const std::vector<TimedToken> tokens = {{0.0, 0.1, "a;b"}};
    const auto packed = packTimedTokens(tokens);
    EXPECT_EQ(packed.find(';'), std::string::npos);
    const auto parsed = parseTimedTokens(packed);
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].token, "a,b");
}

TEST(IxmlTimedTokens, EmptyRoundTrips) {
    EXPECT_TRUE(packTimedTokens({}).empty());
    EXPECT_TRUE(parseTimedTokens("").empty());
}

TEST(IxmlTimedTokens, SkipsMalformedEntries) {
    // Entries missing the two spaces are dropped (with a warning), not turned into
    // garbage tokens. The well-formed neighbour still parses.
    const auto parsed = parseTimedTokens("garbage;0.100 0.200 X;also-bad");
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].token, "X");
    EXPECT_DOUBLE_EQ(parsed[0].start, 0.1);
}
