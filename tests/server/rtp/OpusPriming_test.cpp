#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "server/config.h"
#include "server/rtp/opus/OpusEncoderWrapper.h"
#include "server/rtp/opus/OpusPriming.h"

namespace creatures::rtp::opus {

TEST(OpusPriming, ProducesMatchingReceiverHistoryAndProgramState) {
    Encoder receiverHistoryEncoder;
    Encoder programEncoder;

    const auto transmittedPriming = encodePrimingSequence(receiverHistoryEncoder);
    const auto discardedProgramPriming = encodePrimingSequence(programEncoder);
    EXPECT_EQ(transmittedPriming, discardedProgramPriming);

    std::array<int16_t, RTP_SAMPLES> firstProgramFrame{};
    for (size_t sampleIndex = 0; sampleIndex < firstProgramFrame.size(); ++sampleIndex) {
        firstProgramFrame[sampleIndex] = static_cast<int16_t>((sampleIndex % 97) * 200 - 9600);
    }

    EXPECT_EQ(receiverHistoryEncoder.encode(firstProgramFrame.data()), programEncoder.encode(firstProgramFrame.data()));
}

} // namespace creatures::rtp::opus
