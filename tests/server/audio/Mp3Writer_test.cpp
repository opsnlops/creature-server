
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/audio/Mp3Writer.h"

namespace {

using creatures::audio::encodeMonoToMp3;
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

// A bare MP3 stream starts with an MPEG frame sync (11 set bits: 0xFF followed by 0xEx/0xFx);
// with an ID3v2 tag prepended it starts with "ID3". Accept either as "looks like an MP3".
bool looksLikeMp3(const std::vector<uint8_t> &bytes) {
    if (bytes.size() < 3) {
        return false;
    }
    if (std::memcmp(bytes.data(), "ID3", 3) == 0) {
        return true;
    }
    return bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0;
}

TEST(Mp3Writer, EncodesASineWaveToAValidLookingMp3File) {
    const auto samples = makeSine(440.0, 1.0);
    auto result = encodeMonoToMp3(samples, kShareableSampleRate);
    ASSERT_TRUE(result.isSuccess());

    const auto bytes = result.getValue().value();
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_TRUE(looksLikeMp3(bytes));

    // The whole point: dramatically smaller than the raw PCM.
    EXPECT_LT(bytes.size(), samples.size() * sizeof(int16_t) / 4);
}

TEST(Mp3Writer, OutputIsDeterministicForTheSameInput) {
    const auto samples = makeSine(220.0, 0.25);
    auto first = encodeMonoToMp3(samples, kShareableSampleRate);
    auto second = encodeMonoToMp3(samples, kShareableSampleRate);
    ASSERT_TRUE(first.isSuccess());
    ASSERT_TRUE(second.isSuccess());
    EXPECT_EQ(first.getValue().value(), second.getValue().value());
}

TEST(Mp3Writer, RejectsNon48kSampleRates) {
    const auto samples = makeSine(440.0, 0.1);
    auto result = encodeMonoToMp3(samples, 44100);
    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(creatures::ServerError::InvalidData, result.getError().value().getCode());
}

TEST(Mp3Writer, RejectsEmptyInput) {
    auto result = encodeMonoToMp3({}, kShareableSampleRate);
    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(creatures::ServerError::InvalidData, result.getError().value().getCode());
}

TEST(Mp3Writer, EmbedsProvenanceInId3v2TxxxFrames) {
    const auto samples = makeSine(440.0, 0.1);
    creatures::audio::Id3Comments comments = {
        {"TITLE", "Web Scale"}, {"SOURCE_SCRIPT_ID", "script-9"}, {"DESCRIPTION", "Beaky: hi\nPip: bye"}};
    auto result = encodeMonoToMp3(samples, kShareableSampleRate, creatures::audio::kShareableMp3Bitrate, comments);
    ASSERT_TRUE(result.isSuccess());
    const auto bytes = result.getValue().value();

    // ID3v2 tag comes first, carrying a TXXX frame per key/value pair.
    EXPECT_EQ(0, std::memcmp(bytes.data(), "ID3", 3));
    EXPECT_TRUE(containsBytes(bytes, "TXXX"));
    EXPECT_TRUE(containsBytes(bytes, "TITLE"));
    EXPECT_TRUE(containsBytes(bytes, "Web Scale"));
    EXPECT_TRUE(containsBytes(bytes, "SOURCE_SCRIPT_ID"));
    EXPECT_TRUE(containsBytes(bytes, "script-9"));
    EXPECT_TRUE(containsBytes(bytes, "Beaky: hi\nPip: bye"));
}

TEST(Mp3Writer, HandlesInputShorterThanOneFrame) {
    // 5ms of audio — the encoder should still flush a valid stream.
    const auto samples = makeSine(440.0, 0.005);
    auto result = encodeMonoToMp3(samples, kShareableSampleRate);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(looksLikeMp3(result.getValue().value()));
}

} // namespace
