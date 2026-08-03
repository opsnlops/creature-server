#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include "server/audio/DecodedAudioStream.h"
#include "server/audio/Mp3Writer.h"

namespace creatures::audio {
namespace {

class TemporaryAudioFile {
  public:
    TemporaryAudioFile(std::string extension, const std::vector<uint8_t> &bytes) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("creature-server-decoder-" + std::to_string(unique) + std::move(extension));
        std::ofstream output(path_, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    ~TemporaryAudioFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    [[nodiscard]] const std::filesystem::path &path() const { return path_; }

  private:
    std::filesystem::path path_;
};

std::vector<int16_t> sineWave() {
    std::vector<int16_t> samples(4800);
    for (size_t index = 0; index < samples.size(); ++index) {
        samples[index] = static_cast<int16_t>(10000.0 * std::sin(2.0 * M_PI * 440.0 * index / 48000.0));
    }
    return samples;
}

} // namespace

TEST(DecodedAudioStream, DecodesServerGeneratedMp3InProcess) {
    auto encoded = encodeMonoToMp3(sineWave(), 48000);
    ASSERT_TRUE(encoded.isSuccess());
    const TemporaryAudioFile mp3(".mp3", encoded.getValue().value());

    auto streamResult = DecodedAudioStream::open(mp3.path().string(), 48000, 1);
    ASSERT_TRUE(streamResult.isSuccess());
    std::vector<int16_t> output(1024);
    auto readResult = streamResult.getValue().value()->readFrames(output);
    ASSERT_TRUE(readResult.isSuccess());
    EXPECT_GT(readResult.getValue().value(), 0);
}

TEST(DecodedAudioStream, RejectsMissingFiles) {
    auto result = DecodedAudioStream::open("/definitely/missing/audio.mp3", 48000, 1);
    EXPECT_FALSE(result.isSuccess());
}

} // namespace creatures::audio
