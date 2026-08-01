#pragma once

#include <chrono>
#include <cstdint>

namespace creatures::rtp {

struct RtpWallClockTimestamp {
    uint64_t ntpTimestamp{0};
    uint32_t rtpTimestamp{0};
};

/**
 * Stable mapping between the process monotonic clock, UTC wall time, and an RTP
 * sample clock.
 *
 * Wall time is sampled once at the session epoch. Later mappings advance it
 * using steady_clock so an NTP correction during a track cannot introduce a
 * discontinuity into the media timeline.
 */
class RtpClockMapping {
  public:
    using MonotonicClock = std::chrono::steady_clock;
    using WallClock = std::chrono::system_clock;

    RtpClockMapping(MonotonicClock::time_point monotonicEpoch, WallClock::time_point wallEpoch, uint32_t rtpEpoch,
                    uint32_t sampleRate);

    [[nodiscard]] static RtpClockMapping capture(uint32_t sampleRate);
    [[nodiscard]] RtpWallClockTimestamp at(MonotonicClock::time_point monotonicTime) const;
    [[nodiscard]] uint32_t rtpEpoch() const { return rtpEpoch_; }

  private:
    MonotonicClock::time_point monotonicEpoch_;
    WallClock::time_point wallEpoch_;
    uint32_t rtpEpoch_;
    uint32_t sampleRate_;
};

[[nodiscard]] uint64_t wallClockToNtp(RtpClockMapping::WallClock::time_point wallTime);

} // namespace creatures::rtp
