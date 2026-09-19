#include <gtest/gtest.h>

#include "server/voice/StreamingTTSClient.h"

namespace creatures::voice {

// Issue #189: the duration must follow the sample rate the format names. A
// fixed 44.1 kHz divisor made every pcm_48000 sentence 8.8% too long.
TEST(StreamingTTSDuration, PcmUsesTheRateInTheFormatName) {
    EXPECT_DOUBLE_EQ(estimateAudioSeconds("pcm_48000", 96000), 1.0);
    EXPECT_DOUBLE_EQ(estimateAudioSeconds("pcm_44100", 88200), 1.0);
    EXPECT_DOUBLE_EQ(estimateAudioSeconds("pcm_24000", 48000), 1.0);
    EXPECT_DOUBLE_EQ(estimateAudioSeconds("pcm_48000", 288000), 3.0);
}

TEST(StreamingTTSDuration, Mp3KeepsTheRoughEstimateAndUnknownIsZero) {
    EXPECT_DOUBLE_EQ(estimateAudioSeconds("mp3_44100_192", 24000), 1.0);
    EXPECT_DOUBLE_EQ(estimateAudioSeconds("pcm_", 1000), 0.0);
    EXPECT_DOUBLE_EQ(estimateAudioSeconds("opus_48000", 1000), 0.0);
}

} // namespace creatures::voice
