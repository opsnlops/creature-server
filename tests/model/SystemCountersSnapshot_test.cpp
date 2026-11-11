#include <gtest/gtest.h>

#include "server/metrics/counters.h"

namespace creatures {

TEST(SystemCountersSnapshotTest, PreservesEveryCounterWireKey) {
    SystemCounters counters;
    counters.incrementTotalFrames();
    counters.incrementRtpSendFailures();
    counters.incrementWebsocketMessagesSent();
    counters.setRtpAudioLoadMetrics(1, 2, 3, 4, 5, 6, 7);
    counters.setLocalAudioPlaybackMetrics(8, 9, 10, 11, 12, 13, 14, 15, 16);
    counters.setHttpConnectionMetrics(17, 18);
    // Negative on purpose: every other counter is unsigned, and the PHC offset
    // is the one field that legitimately goes below zero.
    counters.setPhcOffset(true, -6400);

    const auto json = systemCountersSnapshotToJson(counters.snapshot());
    EXPECT_EQ(json.size(), 45);
    EXPECT_EQ(json.at("totalFrames"), 1);
    EXPECT_EQ(json.at("rtpSendFailures"), 1);
    EXPECT_EQ(json.at("websocketMessagesSent"), 1);
    EXPECT_EQ(json.at("rtpAudioLoadsCancelled"), 6);
    EXPECT_EQ(json.at("localAudioPlaybacksTimedOut"), 16);
    EXPECT_EQ(json.at("rtpEncoderResets"), 0);
    EXPECT_EQ(json.at("httpConnectionsActive"), 17);
    EXPECT_EQ(json.at("httpConnectionsRefused"), 18);
    EXPECT_EQ(json.at("phcOffsetNs"), -6400);
    EXPECT_EQ(json.at("phcOffsetAvailable"), true);
}

TEST(SystemCountersSnapshotTest, ReportsNoPhcReadingRatherThanAnOffsetOfZero) {
    // A host with no PHC -- travel mode, or a laptop -- has no reading at all.
    // The zero has to be readable as "nothing measured" rather than as a clock
    // that happens to be perfect, which is what the flag is for.
    const SystemCounters counters;

    const auto json = systemCountersSnapshotToJson(counters.snapshot());
    EXPECT_EQ(json.at("phcOffsetAvailable"), false);
    EXPECT_EQ(json.at("phcOffsetNs"), 0);
}

TEST(SystemCountersSnapshotTest, KeepsTheOffsetSignedThroughTheAtomic) {
    // int64 all the way through: the atomic, the snapshot, and the JSON. A
    // uint64 anywhere in that chain turns -6.4 us into 1.8e19.
    SystemCounters counters;
    counters.setPhcOffset(true, -50360);

    EXPECT_EQ(counters.getPhcOffsetNs(), -50360);
    EXPECT_TRUE(counters.getPhcOffsetAvailable());
    EXPECT_EQ(systemCountersSnapshotToJson(counters.snapshot()).at("phcOffsetNs"), -50360);
}

} // namespace creatures
