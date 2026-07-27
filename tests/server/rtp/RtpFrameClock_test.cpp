#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "server/config.h"
#include "server/rtp/RtpFrameClock.h"

namespace creatures::rtp {

TEST(RtpFrameClock, AdvancesOneTenMillisecondOpusFrameAtATime) {
    RtpFrameClock clock(1000);

    EXPECT_EQ(clock.current(), 1000U);
    clock.advance();
    EXPECT_EQ(clock.current(), 1000U + RTP_SAMPLES);
    clock.advance(4);
    EXPECT_EQ(clock.current(), 1000U + 5U * RTP_SAMPLES);
}

TEST(RtpFrameClock, WrapsOnTheThirtyTwoBitRtpTimeline) {
    RtpFrameClock clock(std::numeric_limits<uint32_t>::max() - 100U);

    clock.advance();

    EXPECT_EQ(clock.current(), static_cast<uint32_t>(std::numeric_limits<uint32_t>::max() - 100U + RTP_SAMPLES));
}

TEST(RtpFrameClock, ResetStartsAFreshStreamTimeline) {
    RtpFrameClock clock(1234);
    clock.advance(10);

    clock.reset(9000);

    EXPECT_EQ(clock.current(), 9000U);
}

} // namespace creatures::rtp
