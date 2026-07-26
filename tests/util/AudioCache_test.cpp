#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "util/AudioCache.h"

namespace creatures::util {
namespace {

namespace fs = std::filesystem;

class AudioCacheTest : public ::testing::Test {
  protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("audio-cache-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        fs::create_directories(root_);
    }

    void TearDown() override {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    fs::path writeSource(const fs::path &relativePath, const std::string &contents) {
        const auto path = root_ / relativePath;
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << contents;
        return path;
    }

    static AudioCache::CachedAudioData makeAudioData(uint8_t marker) {
        AudioCache::CachedAudioData data{};
        data.framesPerChannel = 2;
        for (uint8_t channel = 0; channel < RTP_STREAMING_CHANNELS; ++channel) {
            data.encodedFrames[channel] = {
                {marker, channel, 0},
                {marker, channel, 1},
            };
        }
        return data;
    }

    fs::path root_;
};

TEST_F(AudioCacheTest, RoundTripsVersionedPacketData) {
    const auto source = writeSource("sound\"with-quote.wav", "source-audio");
    AudioCache cache(root_.string());
    const auto expected = makeAudioData(0xA1);

    auto saveResult = cache.saveToCache(source.string(), expected);
    ASSERT_TRUE(saveResult.isSuccess());

    const auto loaded = cache.tryLoadFromCache(source.string());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->framesPerChannel, expected.framesPerChannel);
    EXPECT_EQ(loaded->encodedFrames, expected.encodedFrames);
}

TEST_F(AudioCacheTest, SameBasenameInDifferentDirectoriesDoesNotCollide) {
    const auto firstSource = writeSource("first/shared.wav", "first");
    const auto secondSource = writeSource("second/shared.wav", "second");
    AudioCache cache(root_.string());
    const auto firstData = makeAudioData(0x11);
    const auto secondData = makeAudioData(0x22);

    ASSERT_TRUE(cache.saveToCache(firstSource.string(), firstData).isSuccess());
    ASSERT_TRUE(cache.saveToCache(secondSource.string(), secondData).isSuccess());

    const auto firstLoaded = cache.tryLoadFromCache(firstSource.string());
    const auto secondLoaded = cache.tryLoadFromCache(secondSource.string());
    ASSERT_NE(firstLoaded, nullptr);
    ASSERT_NE(secondLoaded, nullptr);
    EXPECT_EQ(firstLoaded->encodedFrames, firstData.encodedFrames);
    EXPECT_EQ(secondLoaded->encodedFrames, secondData.encodedFrames);
}

TEST_F(AudioCacheTest, RejectsMismatchedChannelFrameCounts) {
    const auto source = writeSource("inconsistent.wav", "source-audio");
    AudioCache cache(root_.string());
    auto inconsistent = makeAudioData(0x33);
    inconsistent.encodedFrames[7].pop_back();

    const auto saveResult = cache.saveToCache(source.string(), inconsistent);

    ASSERT_FALSE(saveResult.isSuccess());
    EXPECT_EQ(saveResult.getError()->getCode(), ServerError::InvalidData);
    EXPECT_EQ(cache.tryLoadFromCache(source.string()), nullptr);
}

TEST_F(AudioCacheTest, ConcurrentReadersHaveRaceFreeStatistics) {
    const auto source = writeSource("concurrent.wav", "concurrent-source");
    AudioCache cache(root_.string());
    const auto expected = makeAudioData(0x42);
    ASSERT_TRUE(cache.saveToCache(source.string(), expected).isSuccess());

    constexpr size_t readerCount = 8;
    std::vector<std::thread> readers;
    std::vector<uint8_t> succeeded(readerCount, 0);
    readers.reserve(readerCount);
    for (size_t index = 0; index < readerCount; ++index) {
        readers.emplace_back([&, index] {
            const auto loaded = cache.tryLoadFromCache(source.string());
            succeeded[index] = loaded && loaded->encodedFrames == expected.encodedFrames ? 1 : 0;
        });
    }
    for (auto &reader : readers) {
        reader.join();
    }

    for (const uint8_t result : succeeded) {
        EXPECT_EQ(result, 1);
    }
    EXPECT_EQ(cache.getStats().cacheHits, readerCount);
}

} // namespace
} // namespace creatures::util
