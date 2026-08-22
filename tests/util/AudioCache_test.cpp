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

    // The verifying overload is the only save path (issue #93): capture the
    // fingerprint the way production does, then publish.
    static Result<void> save(AudioCache &cache, const fs::path &source, const AudioCache::CachedAudioData &data) {
        auto info = cache.getSourceFileInfo(source.string());
        if (!info.isSuccess()) {
            return Result<void>{info.getError().value()};
        }
        return cache.saveToCache(source.string(), data.framesPerChannel, data.encodedFrames, info.getValue().value());
    }

    fs::path root_;
};

TEST_F(AudioCacheTest, RoundTripsVersionedPacketData) {
    const auto source = writeSource("sound\"with-quote.wav", "source-audio");
    AudioCache cache(root_.string());
    const auto expected = makeAudioData(0xA1);

    auto saveResult = save(cache, source, expected);
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

    ASSERT_TRUE(save(cache, firstSource, firstData).isSuccess());
    ASSERT_TRUE(save(cache, secondSource, secondData).isSuccess());

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

    const auto saveResult = save(cache, source, inconsistent);

    ASSERT_FALSE(saveResult.isSuccess());
    EXPECT_EQ(saveResult.getError()->getCode(), ServerError::InvalidData);
    EXPECT_EQ(cache.tryLoadFromCache(source.string()), nullptr);
}

TEST_F(AudioCacheTest, ConcurrentReadersHaveRaceFreeStatistics) {
    const auto source = writeSource("concurrent.wav", "concurrent-source");
    AudioCache cache(root_.string());
    const auto expected = makeAudioData(0x42);
    ASSERT_TRUE(save(cache, source, expected).isSuccess());

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

// --- issue #93: aggregate budget, TOCTOU, cross-process safety ---

TEST_F(AudioCacheTest, RefusesToPublishWhenSourceChangedDuringEncoding) {
    const auto source = writeSource("changing.wav", "original-contents");
    AudioCache cache(root_.string());

    // Fingerprint captured "before the encode"...
    auto beforeResult = cache.getSourceFileInfo(source.string());
    ASSERT_TRUE(beforeResult.isSuccess());
    const auto before = beforeResult.getValue().value();

    // ...then an external writer replaces the WAV mid-encode.
    writeSource("changing.wav", "replacement-contents-of-a-different-length");

    const auto data = makeAudioData(0x55);
    const auto saveResult = cache.saveToCache(source.string(), data.framesPerChannel, data.encodedFrames, before);

    ASSERT_FALSE(saveResult.isSuccess()) << "old packets must not be published under the new file's identity";
    EXPECT_EQ(saveResult.getError()->getCode(), ServerError::InvalidData);
    EXPECT_EQ(cache.tryLoadFromCache(source.string()), nullptr) << "no completion marker may exist";
}

TEST_F(AudioCacheTest, VerifyingSavePublishesWhenSourceIsUnchanged) {
    const auto source = writeSource("stable.wav", "stable-contents");
    AudioCache cache(root_.string());

    auto beforeResult = cache.getSourceFileInfo(source.string());
    ASSERT_TRUE(beforeResult.isSuccess());

    const auto data = makeAudioData(0x66);
    ASSERT_TRUE(
        cache.saveToCache(source.string(), data.framesPerChannel, data.encodedFrames, beforeResult.getValue().value())
            .isSuccess());
    ASSERT_NE(cache.tryLoadFromCache(source.string()), nullptr);
}

TEST_F(AudioCacheTest, RejectsCacheFileDeclaringMoreFramesThanTheCeiling) {
    const auto source = writeSource("huge.wav", "huge-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x77)).isSuccess());
    ASSERT_NE(cache.tryLoadFromCache(source.string()), nullptr);

    // Binary-patch channel 0's frame-count field (right after the metadata
    // block) to a value beyond the duration-derived ceiling. The load must be
    // rejected before any frame allocation happens.
    fs::path channel0;
    for (const auto &entry : fs::recursive_directory_iterator(root_ / ".opus_cache")) {
        if (entry.is_regular_file() && entry.path().filename() == "ch00.opus") {
            channel0 = entry.path();
            break;
        }
    }
    ASSERT_FALSE(channel0.empty());

    std::fstream patch(channel0, std::ios::binary | std::ios::in | std::ios::out);
    uint32_t metadataSize = 0;
    patch.read(reinterpret_cast<char *>(&metadataSize), sizeof(metadataSize));
    ASSERT_TRUE(patch.good());
    patch.seekp(static_cast<std::streamoff>(sizeof(metadataSize) + metadataSize));
    const uint32_t inflated = static_cast<uint32_t>(RTP_MAX_FRAMES_PER_CHANNEL) + 1;
    patch.write(reinterpret_cast<const char *>(&inflated), sizeof(inflated));
    patch.flush();
    ASSERT_TRUE(patch.good());
    patch.close();

    EXPECT_EQ(cache.tryLoadFromCache(source.string()), nullptr);
}

TEST_F(AudioCacheTest, SaveCreatesCrossProcessLockFileAndPidScopedTemps) {
    const auto source = writeSource("locked.wav", "locked-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x88)).isSuccess());

    // The per-key advisory lock file exists next to the channel files, and no
    // bare deterministic ".tmp" leftovers do (temps are pid-scoped and
    // renamed away).
    fs::path lockFile;
    fs::path cacheDir;
    bool sawBareTmp = false;
    for (const auto &entry : fs::recursive_directory_iterator(root_ / ".opus_cache")) {
        const auto name = entry.path().filename().string();
        if (name.size() > 5 && name.substr(name.size() - 5) == ".lock") {
            lockFile = entry.path();
        }
        if (entry.is_directory() && name.find("locked") != std::string::npos) {
            cacheDir = entry.path();
        }
        if (name.size() > 4 && name.substr(name.size() - 4) == ".tmp") {
            sawBareTmp = true;
        }
    }
    ASSERT_FALSE(lockFile.empty());
    EXPECT_FALSE(sawBareTmp);

    // The lock must live OUTSIDE the directory a clear would remove, or a
    // clear unlinks the very inode it holds (issue #93 review).
    ASSERT_FALSE(cacheDir.empty());
    EXPECT_EQ(lockFile.parent_path(), cacheDir.parent_path());
    EXPECT_NE(lockFile.parent_path(), cacheDir);
}

TEST_F(AudioCacheTest, ClearCacheKeepsTheLockFileItHolds) {
    const auto source = writeSource("cleared.wav", "cleared-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x99)).isSuccess());

    fs::path lockFile;
    for (const auto &entry : fs::recursive_directory_iterator(root_ / ".opus_cache")) {
        const auto name = entry.path().filename().string();
        if (name.size() > 5 && name.substr(name.size() - 5) == ".lock") {
            lockFile = entry.path();
        }
    }
    ASSERT_FALSE(lockFile.empty());

    ASSERT_TRUE(cache.clearCache(source.string()).isSuccess());
    EXPECT_TRUE(fs::exists(lockFile)) << "the advisory lock inode must survive a clear";
    EXPECT_EQ(cache.tryLoadFromCache(source.string()), nullptr);
}

TEST_F(AudioCacheTest, SaveSweepsAbandonedTemporariesFromAnEarlierCrash) {
    const auto source = writeSource("swept.wav", "swept-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0xAB)).isSuccess());

    fs::path cacheDir;
    for (const auto &entry : fs::recursive_directory_iterator(root_ / ".opus_cache")) {
        if (entry.is_directory() && entry.path().filename().string().find("swept") != std::string::npos) {
            cacheDir = entry.path();
        }
    }
    ASSERT_FALSE(cacheDir.empty());

    // Simulate a crash mid-save from a previous run: unique temp names never
    // self-clean the way the old deterministic name did (issue #93 review).
    const auto orphan = cacheDir / "ch00.opus.tmp.99999.0";
    {
        std::ofstream abandoned(orphan, std::ios::binary);
        abandoned << "abandoned";
    }
    ASSERT_TRUE(fs::exists(orphan));

    ASSERT_TRUE(save(cache, source, makeAudioData(0xCD)).isSuccess());
    EXPECT_FALSE(fs::exists(orphan)) << "a later save must sweep abandoned temps";
}

} // namespace
} // namespace creatures::util
