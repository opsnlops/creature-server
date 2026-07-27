#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "server/config.h"
#include "server/rtp/opus/OpusEncoderWrapper.h"

namespace creatures::rtp::opus {

using PrimingSequence = std::array<std::vector<uint8_t>, RTP_PRIMING_FRAMES>;

/**
 * Advance an encoder through the exact silence history sent before program
 * audio. Calling this on two fresh, identically configured encoders produces
 * the packet sequence for the receiver and the matching state for frame zero.
 */
inline PrimingSequence encodePrimingSequence(Encoder &encoder) {
    const std::array<int16_t, RTP_SAMPLES> silentFrame{};
    PrimingSequence sequence;
    for (size_t frameIndex = 0; frameIndex < sequence.size(); ++frameIndex) {
        sequence[frameIndex] = encoder.encode(silentFrame.data());
    }
    return sequence;
}

} // namespace creatures::rtp::opus
