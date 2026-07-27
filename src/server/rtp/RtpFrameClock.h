#pragma once

#include <cstddef>
#include <cstdint>

#include "server/config.h"

namespace creatures::rtp {

/**
 * RTP sample clock shared by every channel in a multichannel frame.
 *
 * Opus packets contain RTP_SAMPLES samples, so advancing one encoded frame
 * always advances the clock by exactly 480 at the configured 48kHz/10ms
 * format. Unsigned overflow intentionally follows RTP's 32-bit wraparound.
 */
class RtpFrameClock {
  public:
    explicit RtpFrameClock(uint32_t initialTimestamp = 0) : nextTimestamp_(initialTimestamp) {}

    void reset(uint32_t timestamp) { nextTimestamp_ = timestamp; }

    void advance(size_t frameCount = 1) {
        nextTimestamp_ += static_cast<uint32_t>(frameCount * static_cast<size_t>(RTP_SAMPLES));
    }

    [[nodiscard]] uint32_t current() const { return nextTimestamp_; }

  private:
    uint32_t nextTimestamp_{0};
};

} // namespace creatures::rtp
