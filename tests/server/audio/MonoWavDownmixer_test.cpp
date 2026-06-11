//
// MonoWavDownmixer_test.cpp
// Tests for the travel-mode mono downmix
//

#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "server/audio/MonoWavDownmixer.h"

namespace creatures::audio {

TEST(MonoWavDownmixer, SilenceInSilenceOut) {
    constexpr int channels = 17;
    constexpr size_t frames = 480;
    std::vector<int16_t> input(frames * channels, 0);
    std::vector<int16_t> output(frames, 999); // poison so we notice missed writes

    downmixToMono(input.data(), frames, channels, output.data());

    for (size_t f = 0; f < frames; ++f) {
        EXPECT_EQ(output[f], 0) << "frame " << f;
    }
}

TEST(MonoWavDownmixer, SingleHotChannelPassesThrough) {
    constexpr int channels = 17;
    constexpr size_t frames = 4;
    constexpr int hotChannel = 5;
    constexpr std::array<int16_t, frames> signal = {1000, -2000, 32767, -32768};

    std::vector<int16_t> input(frames * channels, 0);
    for (size_t f = 0; f < frames; ++f) {
        input[f * channels + hotChannel] = signal[f];
    }
    std::vector<int16_t> output(frames, 0);

    downmixToMono(input.data(), frames, channels, output.data());

    for (size_t f = 0; f < frames; ++f) {
        EXPECT_EQ(output[f], signal[f]) << "frame " << f;
    }
}

TEST(MonoWavDownmixer, ClampsInsteadOfWrappingAround) {
    constexpr int channels = 17;
    constexpr size_t frames = 2;

    std::vector<int16_t> input(frames * channels);
    // Frame 0: every channel at positive full scale
    for (int ch = 0; ch < channels; ++ch) {
        input[ch] = 32767;
    }
    // Frame 1: every channel at negative full scale
    for (int ch = 0; ch < channels; ++ch) {
        input[channels + ch] = -32768;
    }
    std::vector<int16_t> output(frames, 0);

    downmixToMono(input.data(), frames, channels, output.data());

    EXPECT_EQ(output[0], 32767);
    EXPECT_EQ(output[1], -32768);
}

TEST(MonoWavDownmixer, SumsAcrossChannelsWithinAFrame) {
    constexpr int channels = 3;
    constexpr size_t frames = 2;

    // Frame 0: 100 + 200 + 300 = 600, Frame 1: -50 + 25 + 0 = -25
    const std::vector<int16_t> input = {100, 200, 300, -50, 25, 0};
    std::vector<int16_t> output(frames, 0);

    downmixToMono(input.data(), frames, channels, output.data());

    EXPECT_EQ(output[0], 600);
    EXPECT_EQ(output[1], -25);
}

TEST(MonoWavDownmixer, FrameIndexingDoesNotBleedBetweenFrames) {
    constexpr int channels = 17;
    constexpr size_t frames = 3;

    std::vector<int16_t> input(frames * channels, 0);
    input[0 * channels + 0] = 10;  // frame 0, first channel
    input[1 * channels + 16] = 20; // frame 1, last channel
    input[2 * channels + 8] = 30;  // frame 2, middle channel
    std::vector<int16_t> output(frames, 0);

    downmixToMono(input.data(), frames, channels, output.data());

    EXPECT_EQ(output[0], 10);
    EXPECT_EQ(output[1], 20);
    EXPECT_EQ(output[2], 30);
}

TEST(MonoWavDownmixer, StereoInputWorks) {
    constexpr int channels = 2;
    constexpr size_t frames = 2;

    const std::vector<int16_t> input = {1000, 2000, -500, -1500};
    std::vector<int16_t> output(frames, 0);

    downmixToMono(input.data(), frames, channels, output.data());

    EXPECT_EQ(output[0], 3000);
    EXPECT_EQ(output[1], -2000);
}

TEST(MonoWavDownmixer, MonoInputIsUnchanged) {
    constexpr int channels = 1;
    constexpr size_t frames = 4;

    const std::vector<int16_t> input = {1, -2, 32767, -32768};
    std::vector<int16_t> output(frames, 0);

    downmixToMono(input.data(), frames, channels, output.data());

    for (size_t f = 0; f < frames; ++f) {
        EXPECT_EQ(output[f], input[f]) << "frame " << f;
    }
}

TEST(MonoWavDownmixer, LoadWavAsMonoRejectsMissingFile) {
    auto result = loadWavAsMono("/nonexistent/path/to/nothing.wav");
    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(result.getError()->getCode(), ServerError::NotFound);
}

} // namespace creatures::audio
