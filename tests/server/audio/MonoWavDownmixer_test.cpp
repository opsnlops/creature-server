//
// MonoWavDownmixer_test.cpp
// Tests for the travel-mode mono downmix
//

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/audio/MonoWavDownmixer.h"

namespace creatures::audio {

namespace {

class TemporaryPcmWav {
  public:
    TemporaryPcmWav(uint16_t channels, uint32_t sampleRate, const std::vector<int16_t> &samples,
                    bool extensible = false, bool includeOddSizedJunkChunk = false,
                    std::optional<uint32_t> declaredDataSize = std::nullopt) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ =
            std::filesystem::temp_directory_path() / ("creature-server-mono-stream-" + std::to_string(unique) + ".wav");

        std::ofstream file(path_, std::ios::binary);
        const uint32_t formatSize = extensible ? 40 : 16;
        const uint32_t junkStorageSize = includeOddSizedJunkChunk ? 12 : 0;
        const uint32_t dataSize = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
        const uint32_t riffSize = 4 + junkStorageSize + 8 + formatSize + 8 + dataSize;

        writeText(file, "RIFF");
        writeU32(file, riffSize);
        writeText(file, "WAVE");

        if (includeOddSizedJunkChunk) {
            writeText(file, "JUNK");
            writeU32(file, 3);
            file.write("abc", 3);
            file.put('\0');
        }

        writeText(file, "fmt ");
        writeU32(file, formatSize);
        writeU16(file, extensible ? 0xFFFE : 0x0001);
        writeU16(file, channels);
        writeU32(file, sampleRate);
        writeU32(file, sampleRate * channels * sizeof(int16_t));
        writeU16(file, static_cast<uint16_t>(channels * sizeof(int16_t)));
        writeU16(file, 16);

        if (extensible) {
            writeU16(file, 22); // extension size
            writeU16(file, 16); // valid bits per sample
            writeU32(file, 0);  // channel mask is not needed by the server
            static constexpr std::array<uint8_t, 16> PCM_SUBFORMAT = {
                0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
            };
            file.write(reinterpret_cast<const char *>(PCM_SUBFORMAT.data()), PCM_SUBFORMAT.size());
        }

        writeText(file, "data");
        writeU32(file, declaredDataSize.value_or(dataSize));
        for (const int16_t sample : samples) {
            writeU16(file, static_cast<uint16_t>(sample));
        }
    }

    ~TemporaryPcmWav() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    [[nodiscard]] const std::filesystem::path &path() const { return path_; }

  private:
    static void writeText(std::ofstream &file, const char *text) { file.write(text, 4); }

    static void writeU16(std::ofstream &file, uint16_t value) {
        const std::array<uint8_t, 2> bytes = {
            static_cast<uint8_t>(value),
            static_cast<uint8_t>(value >> 8),
        };
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }

    static void writeU32(std::ofstream &file, uint32_t value) {
        const std::array<uint8_t, 4> bytes = {
            static_cast<uint8_t>(value),
            static_cast<uint8_t>(value >> 8),
            static_cast<uint8_t>(value >> 16),
            static_cast<uint8_t>(value >> 24),
        };
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }

    std::filesystem::path path_;
};

} // namespace

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

TEST(MonoWavDownmixer, StreamsPcmInCallerSizedChunks) {
    const TemporaryPcmWav wav(3, 48000,
                              {
                                  100,
                                  200,
                                  300,
                                  -50,
                                  25,
                                  0,
                                  32767,
                                  1,
                                  1,
                                  -32768,
                                  -1,
                                  -1,
                              });

    auto openResult = MonoWavStream::open(wav.path().string());
    ASSERT_TRUE(openResult.isSuccess());
    const auto stream = openResult.getValue().value();
    EXPECT_EQ(stream->channels(), 3);
    EXPECT_EQ(stream->sampleRate(), 48000);
    EXPECT_EQ(stream->totalFrames(), 4);

    std::array<int16_t, 2> firstChunk{};
    auto firstRead = stream->readMonoFrames(firstChunk);
    ASSERT_TRUE(firstRead.isSuccess());
    EXPECT_EQ(firstRead.getValue().value(), 2);
    EXPECT_EQ(firstChunk[0], 600);
    EXPECT_EQ(firstChunk[1], -25);

    std::array<int16_t, 3> secondChunk{};
    auto secondRead = stream->readMonoFrames(secondChunk);
    ASSERT_TRUE(secondRead.isSuccess());
    EXPECT_EQ(secondRead.getValue().value(), 2);
    EXPECT_EQ(secondChunk[0], 32767);
    EXPECT_EQ(secondChunk[1], -32768);

    auto endRead = stream->readMonoFrames(secondChunk);
    ASSERT_TRUE(endRead.isSuccess());
    EXPECT_EQ(endRead.getValue().value(), 0);
}

TEST(MonoWavDownmixer, StreamingReaderSkipsPaddedUnknownChunks) {
    const TemporaryPcmWav wav(1, 24000, {123, -456}, false, true);

    auto openResult = MonoWavStream::open(wav.path().string());
    ASSERT_TRUE(openResult.isSuccess());
    const auto stream = openResult.getValue().value();

    std::array<int16_t, 2> output{};
    auto readResult = stream->readMonoFrames(output);
    ASSERT_TRUE(readResult.isSuccess());
    EXPECT_EQ(readResult.getValue().value(), 2);
    EXPECT_EQ(output[0], 123);
    EXPECT_EQ(output[1], -456);
}

TEST(MonoWavDownmixer, StreamingReaderCapsEachSourceRead) {
    std::vector<int16_t> samples(5000, 7);
    const TemporaryPcmWav wav(1, 48000, samples);

    auto openResult = MonoWavStream::open(wav.path().string());
    ASSERT_TRUE(openResult.isSuccess());
    const auto stream = openResult.getValue().value();

    std::vector<int16_t> output(samples.size());
    auto firstRead = stream->readMonoFrames(output);
    ASSERT_TRUE(firstRead.isSuccess());
    EXPECT_EQ(firstRead.getValue().value(), 4096);

    auto secondRead = stream->readMonoFrames(std::span<int16_t>{output}.subspan(4096));
    ASSERT_TRUE(secondRead.isSuccess());
    EXPECT_EQ(secondRead.getValue().value(), 904);
}

TEST(MonoWavDownmixer, StreamingReaderAcceptsExtensibleSeventeenChannelPcm) {
    std::vector<int16_t> samples(34, 0);
    samples[5] = 1200;
    samples[17 + 16] = -800;
    const TemporaryPcmWav wav(17, 48000, samples, true);

    auto openResult = MonoWavStream::open(wav.path().string());
    ASSERT_TRUE(openResult.isSuccess());
    const auto stream = openResult.getValue().value();
    EXPECT_EQ(stream->channels(), 17);

    std::array<int16_t, 2> output{};
    auto readResult = stream->readMonoFrames(output);
    ASSERT_TRUE(readResult.isSuccess());
    EXPECT_EQ(readResult.getValue().value(), 2);
    EXPECT_EQ(output[0], 1200);
    EXPECT_EQ(output[1], -800);
}

TEST(MonoWavDownmixer, WholeFileHelperUsesStreamingReader) {
    const TemporaryPcmWav wav(2, 48000, {100, 200, -50, -150});

    auto result = loadWavAsMono(wav.path().string());
    ASSERT_TRUE(result.isSuccess());
    const auto mono = result.getValue().value();
    EXPECT_EQ(mono.sampleRate, 48000);
    ASSERT_EQ(mono.samples.size(), 2);
    EXPECT_EQ(mono.samples[0], 300);
    EXPECT_EQ(mono.samples[1], -200);
}

TEST(MonoWavDownmixer, WholeFileHelperRejectsFrameLimitBeforeLoadingSamples) {
    const TemporaryPcmWav wav(1, 48000, {1, 2, 3, 4});

    const auto result = loadWavAsMono(wav.path().string(), 3);

    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(result.getError()->getCode(), ServerError::InvalidData);
}

TEST(MonoWavDownmixer, RejectsAChunkDeclaredPastEndOfFileBeforeAllocating) {
    const TemporaryPcmWav wav(1, 48000, {123}, false, false, std::numeric_limits<uint32_t>::max());

    const auto result = MonoWavStream::open(wav.path().string());

    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(result.getError()->getCode(), creatures::ServerError::InvalidData);
}

} // namespace creatures::audio
