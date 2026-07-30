#include "server/rtp/RtpClockMapping.h"

#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace creatures::rtp {
namespace {

constexpr int64_t NTP_UNIX_EPOCH_OFFSET_SECONDS = 2'208'988'800LL;
constexpr uint64_t NTP_FRACTION_SCALE = uint64_t{1} << 32U;
constexpr int64_t NANOSECONDS_PER_SECOND = 1'000'000'000LL;

uint32_t samplesForDuration(std::chrono::nanoseconds elapsed, uint32_t sampleRate) {
    const auto wholeSeconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
    const auto remainder = elapsed - wholeSeconds;
    const int64_t wholeSamples = wholeSeconds.count() * static_cast<int64_t>(sampleRate);
    const int64_t fractionalSamples = remainder.count() * static_cast<int64_t>(sampleRate) / NANOSECONDS_PER_SECOND;
    return static_cast<uint32_t>(wholeSamples + fractionalSamples);
}

} // namespace

RtpClockMapping::RtpClockMapping(MonotonicClock::time_point monotonicEpoch, WallClock::time_point wallEpoch,
                                 uint32_t rtpEpoch, uint32_t sampleRate)
    : monotonicEpoch_(monotonicEpoch), wallEpoch_(wallEpoch), rtpEpoch_(rtpEpoch), sampleRate_(sampleRate) {
    if (sampleRate_ == 0) {
        throw std::invalid_argument("RTP clock mapping requires a non-zero sample rate");
    }
}

RtpClockMapping RtpClockMapping::capture(uint32_t sampleRate) {
    if (sampleRate == 0) {
        throw std::invalid_argument("RTP clock mapping requires a non-zero sample rate");
    }

    // Bracket the wall-clock read and use the monotonic midpoint to minimize
    // phase error between the two clocks.
    const auto monotonicBefore = MonotonicClock::now();
    const auto wallEpoch = WallClock::now();
    const auto monotonicAfter = MonotonicClock::now();
    const auto monotonicEpoch = monotonicBefore + (monotonicAfter - monotonicBefore) / 2;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(monotonicEpoch.time_since_epoch());
    const uint32_t rtpEpoch = samplesForDuration(elapsed, sampleRate);

    return RtpClockMapping(monotonicEpoch, wallEpoch, rtpEpoch, sampleRate);
}

RtpWallClockTimestamp RtpClockMapping::at(MonotonicClock::time_point monotonicTime) const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(monotonicTime - monotonicEpoch_);
    return {
        wallClockToNtp(std::chrono::time_point_cast<WallClock::duration>(wallEpoch_ + elapsed)),
        static_cast<uint32_t>(rtpEpoch_ + samplesForDuration(elapsed, sampleRate_)),
    };
}

uint64_t wallClockToNtp(RtpClockMapping::WallClock::time_point wallTime) {
    const auto sinceUnixEpoch = std::chrono::duration_cast<std::chrono::nanoseconds>(wallTime.time_since_epoch());
    auto wholeSeconds = std::chrono::duration_cast<std::chrono::seconds>(sinceUnixEpoch);
    auto remainder = sinceUnixEpoch - wholeSeconds;
    if (remainder.count() < 0) {
        wholeSeconds -= std::chrono::seconds(1);
        remainder += std::chrono::seconds(1);
    }

    const int64_t ntpSeconds = wholeSeconds.count() + NTP_UNIX_EPOCH_OFFSET_SECONDS;
    if (ntpSeconds < 0) {
        throw std::out_of_range("wall time predates the NTP epoch");
    }

    const uint64_t fraction = static_cast<uint64_t>(remainder.count()) * NTP_FRACTION_SCALE / NANOSECONDS_PER_SECOND;
    return (static_cast<uint64_t>(ntpSeconds) << 32U) | fraction;
}

} // namespace creatures::rtp
