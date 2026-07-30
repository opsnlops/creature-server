#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

#include "server/config.h"
#include "server/rtp/RtpClockMapping.h"
#include "server/rtp/RtpFrameClock.h"

namespace creatures::rtp {
namespace {

using namespace std::chrono_literals;

TEST(RtpClockMapping, ConvertsUnixEpochToNtpEpoch) {
    const auto unixEpoch = RtpClockMapping::WallClock::time_point{};

    EXPECT_EQ(wallClockToNtp(unixEpoch), uint64_t{2'208'988'800} << 32U);
    EXPECT_EQ(wallClockToNtp(unixEpoch + 500ms), (uint64_t{2'208'988'800} << 32U) | uint64_t{0x8000'0000});
}

TEST(RtpClockMapping, AdvancesWallAndRtpClocksFromOneEpoch) {
    const auto monotonicEpoch = RtpClockMapping::MonotonicClock::time_point{} + 10s;
    const auto wallEpoch = RtpClockMapping::WallClock::time_point{} + 1'000s;
    const RtpClockMapping mapping(monotonicEpoch, wallEpoch, 12'345U, 48'000U);

    const auto timestamp = mapping.at(monotonicEpoch + 1s + 250ms);

    EXPECT_EQ(timestamp.ntpTimestamp, wallClockToNtp(wallEpoch + 1s + 250ms));
    EXPECT_EQ(timestamp.rtpTimestamp, 12'345U + 60'000U);
}

TEST(RtpClockMapping, PreservesSubMillisecondRtpPrecision) {
    const auto monotonicEpoch = RtpClockMapping::MonotonicClock::time_point{};
    const RtpClockMapping mapping(monotonicEpoch, RtpClockMapping::WallClock::time_point{}, 100U, 48'000U);

    const auto timestamp = mapping.at(monotonicEpoch + 1500us);

    EXPECT_EQ(timestamp.rtpTimestamp, 172U);
}

TEST(RtpClockMapping, WrapsTheRtpTimestampWithoutDisturbingNtp) {
    const auto monotonicEpoch = RtpClockMapping::MonotonicClock::time_point{};
    const auto wallEpoch = RtpClockMapping::WallClock::time_point{} + 1'000s;
    const RtpClockMapping mapping(monotonicEpoch, wallEpoch, std::numeric_limits<uint32_t>::max() - 100U, 48'000U);

    const auto timestamp = mapping.at(monotonicEpoch + 10ms);

    EXPECT_EQ(timestamp.rtpTimestamp, static_cast<uint32_t>(std::numeric_limits<uint32_t>::max() - 100U + 480U));
    EXPECT_EQ(timestamp.ntpTimestamp, wallClockToNtp(wallEpoch + 10ms));
}

TEST(RtpClockMapping, MatchesFrameClockWhenLateFramesAreSkipped) {
    const auto monotonicEpoch = RtpClockMapping::MonotonicClock::time_point{};
    const RtpClockMapping mapping(monotonicEpoch, RtpClockMapping::WallClock::time_point{}, 50'000U, RTP_SRATE);
    RtpFrameClock frameClock(mapping.rtpEpoch());

    frameClock.advance(3); // Three late frames were not sent.
    frameClock.advance();  // The next frame was sent on the shared timeline.

    EXPECT_EQ(frameClock.current(), mapping.at(monotonicEpoch + 40ms).rtpTimestamp);
}

TEST(RtpClockMapping, RejectsAZeroSampleRate) {
    EXPECT_THROW(
        RtpClockMapping(RtpClockMapping::MonotonicClock::time_point{}, RtpClockMapping::WallClock::time_point{}, 0, 0),
        std::invalid_argument);
}

} // namespace
} // namespace creatures::rtp
