
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/audio/OggOpusWriter.h"

namespace {

using creatures::audio::encodeMonoToOggOpus;
using creatures::audio::kShareableSampleRate;

std::vector<int16_t> makeSine(double frequencyHz, double seconds) {
    const auto sampleCount = static_cast<size_t>(seconds * kShareableSampleRate);
    std::vector<int16_t> samples(sampleCount);
    for (size_t i = 0; i < sampleCount; i++) {
        const double t = static_cast<double>(i) / kShareableSampleRate;
        samples[i] = static_cast<int16_t>(12000.0 * std::sin(2.0 * M_PI * frequencyHz * t));
    }
    return samples;
}

bool containsBytes(const std::vector<uint8_t> &haystack, const std::string &needle) {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

TEST(OggOpusWriter, EncodesASineWaveToAValidLookingOggOpusFile) {
    const auto samples = makeSine(440.0, 1.0);
    auto result = encodeMonoToOggOpus(samples, kShareableSampleRate);
    ASSERT_TRUE(result.isSuccess());

    const auto bytes = result.getValue().value();
    ASSERT_GE(bytes.size(), 4u);

    // Starts with an Ogg page and carries the RFC 7845 headers.
    EXPECT_EQ(0, std::memcmp(bytes.data(), "OggS", 4));
    EXPECT_TRUE(containsBytes(bytes, "OpusHead"));
    EXPECT_TRUE(containsBytes(bytes, "OpusTags"));

    // The whole point: dramatically smaller than the raw PCM.
    EXPECT_LT(bytes.size(), samples.size() * sizeof(int16_t) / 4);
}

TEST(OggOpusWriter, OutputIsDeterministicForTheSameInput) {
    const auto samples = makeSine(220.0, 0.25);
    auto first = encodeMonoToOggOpus(samples, kShareableSampleRate);
    auto second = encodeMonoToOggOpus(samples, kShareableSampleRate);
    ASSERT_TRUE(first.isSuccess());
    ASSERT_TRUE(second.isSuccess());
    EXPECT_EQ(first.getValue().value(), second.getValue().value());
}

TEST(OggOpusWriter, RejectsNon48kSampleRates) {
    const auto samples = makeSine(440.0, 0.1);
    auto result = encodeMonoToOggOpus(samples, 44100);
    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(creatures::ServerError::InvalidData, result.getError().value().getCode());
}

TEST(OggOpusWriter, RejectsEmptyInput) {
    auto result = encodeMonoToOggOpus({}, kShareableSampleRate);
    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(creatures::ServerError::InvalidData, result.getError().value().getCode());
}

TEST(OggOpusWriter, HandlesInputShorterThanOneFrame) {
    // 5ms of audio — less than the 20ms frame; the tail-padding path.
    const auto samples = makeSine(440.0, 0.005);
    auto result = encodeMonoToOggOpus(samples, kShareableSampleRate);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(0, std::memcmp(result.getValue().value().data(), "OggS", 4));
}

} // namespace
