#include <algorithm>
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

// --- issue #166: pruning entries that can never be used again ---

TEST_F(AudioCacheTest, PruneReclaimsEntriesWhoseSourceIsGone) {
    const auto keep = writeSource("keep.wav", "keep-source");
    const auto doomed = writeSource("doomed.wav", "doomed-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, keep, makeAudioData(0x01)).isSuccess());
    ASSERT_TRUE(save(cache, doomed, makeAudioData(0x02)).isSuccess());

    // Simulate the operator moving/deleting a sound: the cache key is a hash
    // of the old canonical path, so its entry can never be reached again.
    fs::remove(doomed);

    // Dry run first: reports but changes nothing.
    auto preview = cache.pruneOrphanedEntries(/*dryRun=*/true);
    ASSERT_TRUE(preview.isSuccess());
    EXPECT_TRUE(preview.getValue().value().dryRun);
    EXPECT_EQ(preview.getValue().value().orphanedEntries, 1U);
    EXPECT_GT(preview.getValue().value().bytesReclaimed, 0U);
    EXPECT_NE(cache.tryLoadFromCache(keep.string()), nullptr) << "dry run must not delete anything";

    auto applied = cache.pruneOrphanedEntries(/*dryRun=*/false);
    ASSERT_TRUE(applied.isSuccess());
    const auto report = applied.getValue().value();
    EXPECT_FALSE(report.dryRun);
    EXPECT_EQ(report.orphanedEntries, 1U);

    // The surviving entry is untouched and still a cache hit.
    EXPECT_NE(cache.tryLoadFromCache(keep.string()), nullptr);
}

TEST_F(AudioCacheTest, PruneRemovesEntriesLeftIncompleteByACrashedSave) {
    const auto source = writeSource("crashed.wav", "crashed-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x03)).isSuccess());

    fs::path cacheDir;
    for (const auto &entry : fs::recursive_directory_iterator(root_ / ".opus_cache")) {
        if (entry.is_directory() && entry.path().filename().string().find("crashed") != std::string::npos) {
            cacheDir = entry.path();
        }
    }
    ASSERT_FALSE(cacheDir.empty());

    // A save that died before writing the completion marker: unreadable by
    // design (allCacheFilesExist checks the marker first), so it is pure waste.
    fs::remove(cacheDir / ".complete");

    auto applied = cache.pruneOrphanedEntries(/*dryRun=*/false);
    ASSERT_TRUE(applied.isSuccess());
    EXPECT_EQ(applied.getValue().value().incompleteEntries, 1U);
    EXPECT_FALSE(fs::exists(cacheDir));
}

TEST_F(AudioCacheTest, PruneSweepsAbandonedTemporariesAndStaleLocks) {
    const auto source = writeSource("tidy.wav", "tidy-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x04)).isSuccess());

    const auto hostRoot = [&] {
        for (const auto &entry : fs::directory_iterator(root_ / ".opus_cache")) {
            if (entry.is_directory()) {
                return entry.path();
            }
        }
        return fs::path{};
    }();
    ASSERT_FALSE(hostRoot.empty());

    // A real abandoned temp lives INSIDE the key's directory — that is where
    // saveToCache writes them (<dir>/chNN.opus.tmp.<pid>.<n>). A lock file
    // whose directory was cleared sits beside it at the host root, since locks
    // are siblings of the directory by design (issue #93).
    fs::path liveEntryDir;
    for (const auto &entry : fs::directory_iterator(hostRoot)) {
        if (entry.is_directory() && entry.path().filename().string().find("tidy") != std::string::npos) {
            liveEntryDir = entry.path();
        }
    }
    ASSERT_FALSE(liveEntryDir.empty());

    const auto orphanTemp = liveEntryDir / "ch03.opus.tmp.4242.7";
    const auto orphanLock = hostRoot / "long-gone-entry_deadbeefdeadbeef.lock";
    {
        std::ofstream(orphanTemp, std::ios::binary) << "junk";
    }
    {
        std::ofstream(orphanLock, std::ios::binary) << "";
    }
    ASSERT_TRUE(fs::exists(orphanTemp));
    ASSERT_TRUE(fs::exists(orphanLock));

    auto applied = cache.pruneOrphanedEntries(/*dryRun=*/false);
    ASSERT_TRUE(applied.isSuccess());
    const auto report = applied.getValue().value();
    EXPECT_EQ(report.temporaryFiles, 1U);
    EXPECT_EQ(report.orphanedLockFiles, 1U);
    EXPECT_FALSE(fs::exists(orphanTemp));
    EXPECT_FALSE(fs::exists(orphanLock));

    // The live entry — and the lock file it still needs — survive.
    EXPECT_NE(cache.tryLoadFromCache(source.string()), nullptr);
}

TEST_F(AudioCacheTest, PruneLeavesAHealthyCacheCompletelyAlone) {
    const auto first = writeSource("healthy-one.wav", "one");
    const auto second = writeSource("healthy-two.wav", "two");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, first, makeAudioData(0x05)).isSuccess());
    ASSERT_TRUE(save(cache, second, makeAudioData(0x06)).isSuccess());

    auto applied = cache.pruneOrphanedEntries(/*dryRun=*/false);
    ASSERT_TRUE(applied.isSuccess());
    const auto report = applied.getValue().value();
    EXPECT_EQ(report.entriesScanned, 2U);
    EXPECT_EQ(report.orphanedEntries, 0U);
    EXPECT_EQ(report.incompleteEntries, 0U);
    EXPECT_EQ(report.bytesReclaimed, 0U);
    EXPECT_TRUE(report.removed.empty());

    EXPECT_NE(cache.tryLoadFromCache(first.string()), nullptr);
    EXPECT_NE(cache.tryLoadFromCache(second.string()), nullptr);
}

TEST_F(AudioCacheTest, PruneKeepsEntriesWhoseSourceCannotBeStatted) {
    // A source on a briefly-unavailable mount makes exists() FAIL rather than
    // cleanly report "not there". Deleting on an inconclusive answer would
    // wipe a healthy cache, so the entry must be left alone (issue #166).
    const auto unreachableDir = root_ / "mounted";
    fs::create_directories(unreachableDir);
    const auto source = writeSource("mounted/on-a-mount.wav", "mounted-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x07)).isSuccess());
    ASSERT_NE(cache.tryLoadFromCache(source.string()), nullptr);

    // Drop search permission so stat on the child errors with EACCES.
    fs::permissions(unreachableDir, fs::perms::none, fs::perm_options::replace);
    auto applied = cache.pruneOrphanedEntries(/*dryRun=*/false);
    // Restore before asserting, so a failure cannot leave an unremovable tree.
    fs::permissions(unreachableDir, fs::perms::owner_all, fs::perm_options::replace);

    ASSERT_TRUE(applied.isSuccess());
    EXPECT_EQ(applied.getValue().value().orphanedEntries, 0U)
        << "an inconclusive stat must never be treated as a missing source";
    EXPECT_NE(cache.tryLoadFromCache(source.string()), nullptr) << "the healthy entry must survive";
}

TEST_F(AudioCacheTest, DryRunCreatesNoFilesAtAll) {
    // A dry run must be strictly read-only. The advisory lock is opened with
    // O_CREAT, so taking it during a preview silently littered one empty .lock
    // file per cache entry — observed in production, where a dry run reported
    // zero stale locks and the following apply removed one per entry it
    // deleted (issue #168).
    const auto source = writeSource("preview.wav", "preview-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x0A)).isSuccess());

    // A legacy-layout entry: a cache directory that predates advisory locking,
    // so it has NO sibling .lock. This is what production was full of, and the
    // only shape that exposes the bug — an entry saved by current code already
    // has its lock file, leaving nothing for a dry run to create.
    fs::path hostRoot;
    for (const auto &entry : fs::directory_iterator(root_ / ".opus_cache")) {
        if (entry.is_directory()) {
            hostRoot = entry.path();
        }
    }
    ASSERT_FALSE(hostRoot.empty());
    const auto legacyEntry = hostRoot / "ancient-entry-no-hash";
    fs::create_directories(legacyEntry);
    {
        std::ofstream stub(legacyEntry / "ch00.opus", std::ios::binary);
        stub << "legacy";
    }
    ASSERT_FALSE(fs::exists(legacyEntry.string() + ".lock"));

    auto snapshot = [&] {
        std::vector<std::string> paths;
        for (const auto &entry : fs::recursive_directory_iterator(root_ / ".opus_cache")) {
            paths.push_back(entry.path().string());
        }
        std::sort(paths.begin(), paths.end());
        return paths;
    };

    const auto before = snapshot();
    ASSERT_TRUE(cache.pruneOrphanedEntries(/*dryRun=*/true).isSuccess());
    const auto after = snapshot();

    EXPECT_EQ(before, after) << "a dry run must not create, remove, or touch any file";
    EXPECT_FALSE(fs::exists(legacyEntry.string() + ".lock"))
        << "previewing an entry must not leave an advisory lock file behind";
}

TEST_F(AudioCacheTest, DryRunStillReportsAbandonedTemporaries) {
    // The temp sweep must be counted during a preview even though nothing is
    // deleted — otherwise the dry run under-reports what an apply would do.
    const auto source = writeSource("previewtemp.wav", "preview-temp-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x0B)).isSuccess());

    fs::path entryDir;
    for (const auto &entry : fs::recursive_directory_iterator(root_ / ".opus_cache")) {
        if (entry.is_directory() && entry.path().filename().string().find("previewtemp") != std::string::npos) {
            entryDir = entry.path();
        }
    }
    ASSERT_FALSE(entryDir.empty());
    const auto orphanTemp = entryDir / "ch05.opus.tmp.1234.9";
    {
        std::ofstream temp(orphanTemp, std::ios::binary);
        temp << "junk";
    }

    auto preview = cache.pruneOrphanedEntries(/*dryRun=*/true);
    ASSERT_TRUE(preview.isSuccess());
    EXPECT_EQ(preview.getValue().value().temporaryFiles, 1U) << "preview must report temps it would remove";
    EXPECT_TRUE(fs::exists(orphanTemp)) << "but must not actually remove them";
}

} // namespace
} // namespace creatures::util
