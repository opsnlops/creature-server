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

    const auto json = systemCountersSnapshotToJson(counters.snapshot());
    EXPECT_EQ(json.size(), 43);
    EXPECT_EQ(json.at("totalFrames"), 1);
    EXPECT_EQ(json.at("rtpSendFailures"), 1);
    EXPECT_EQ(json.at("websocketMessagesSent"), 1);
    EXPECT_EQ(json.at("rtpAudioLoadsCancelled"), 6);
    EXPECT_EQ(json.at("localAudioPlaybacksTimedOut"), 16);
    EXPECT_EQ(json.at("rtpEncoderResets"), 0);
    EXPECT_EQ(json.at("httpConnectionsActive"), 17);
    EXPECT_EQ(json.at("httpConnectionsRefused"), 18);
}

} // namespace creatures
