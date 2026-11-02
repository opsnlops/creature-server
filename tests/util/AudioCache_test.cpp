#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
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

    // The hostname-scoped directory the cache actually writes into.
    fs::path hostRoot() const {
        for (const auto &entry : fs::directory_iterator(root_ / ".opus_cache")) {
            if (entry.is_directory()) {
                return entry.path();
            }
        }
        return {};
    }

    // The one cache entry whose directory name mentions `hint`.
    fs::path entryDirFor(const std::string &hint) const {
        for (const auto &entry : fs::directory_iterator(hostRoot())) {
            if (entry.is_directory() && entry.path().filename().string().find(hint) != std::string::npos) {
                return entry.path();
            }
        }
        return {};
    }

    // Backdate an entry's last-read stamp. Production stamps the `.complete`
    // marker on every cache read (issue #225), so this is how a test says
    // "nothing has touched this entry in a while".
    static void backdateLastRead(const fs::path &entryDir, std::chrono::seconds age) {
        fs::last_write_time(entryDir / ".complete", fs::file_time_type::clock::now() - age);
    }

    static AudioCache::PrunePolicy idleAfter(std::chrono::seconds age) {
        AudioCache::PrunePolicy policy;
        policy.maxIdleAge = age;
        return policy;
    }

    static AudioCache::PrunePolicy cappedAt(std::uintmax_t bytes) {
        AudioCache::PrunePolicy policy;
        policy.maxTotalBytes = bytes;
        return policy;
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

// --- issue #225: automatic prune policy (idle eviction + size cap) ---

TEST_F(AudioCacheTest, ReadingAnEntryStampsItAsRecentlyUsed) {
    // Last-use tracking cannot lean on atime (noatime/relatime mounts), so a
    // cache read touches the `.complete` marker instead. Without this the
    // idle tier would evict entries the show plays every night.
    const auto source = writeSource("stamped.wav", "stamped-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x20)).isSuccess());

    const auto entryDir = entryDirFor("stamped");
    ASSERT_FALSE(entryDir.empty());
    backdateLastRead(entryDir, std::chrono::hours(72));
    const auto before = fs::last_write_time(entryDir / ".complete");

    ASSERT_NE(cache.tryLoadFromCache(source.string()), nullptr);

    EXPECT_GT(fs::last_write_time(entryDir / ".complete"), before)
        << "a cache read must stamp the entry so a prune sees it as live";
}

TEST_F(AudioCacheTest, PruneEvictsEntriesIdleLongerThanThePolicy) {
    const auto stale = writeSource("stale.wav", "stale-source");
    const auto fresh = writeSource("fresh.wav", "fresh-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, stale, makeAudioData(0x21)).isSuccess());
    ASSERT_TRUE(save(cache, fresh, makeAudioData(0x22)).isSuccess());

    const auto staleDir = entryDirFor("stale");
    ASSERT_FALSE(staleDir.empty());
    backdateLastRead(staleDir, std::chrono::hours(72));

    auto applied = cache.prune(idleAfter(std::chrono::hours(24)), /*dryRun=*/false);
    ASSERT_TRUE(applied.isSuccess());
    const auto report = applied.getValue().value();

    EXPECT_EQ(report.idleEntries, 1U);
    EXPECT_EQ(report.orphanedEntries, 0U);
    EXPECT_FALSE(fs::exists(staleDir)) << "the idle entry should be gone";
    EXPECT_NE(cache.tryLoadFromCache(fresh.string()), nullptr) << "the recently-read entry must survive";
    EXPECT_GT(report.bytesReclaimed, 0U);
}

TEST_F(AudioCacheTest, OrphanOnlyPruneNeverEvictsAUsableIdleEntry) {
    // pruneOrphanedEntries() is the historical, policy-free entry point and
    // the manual fallback when no sweep is configured. It must stay purely
    // about entries that can never be used again.
    const auto source = writeSource("ancient.wav", "ancient-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x23)).isSuccess());
    backdateLastRead(entryDirFor("ancient"), std::chrono::hours(24 * 400));

    auto applied = cache.pruneOrphanedEntries(/*dryRun=*/false);
    ASSERT_TRUE(applied.isSuccess());

    EXPECT_EQ(applied.getValue().value().idleEntries, 0U);
    EXPECT_EQ(applied.getValue().value().oversizeEntries, 0U);
    EXPECT_NE(cache.tryLoadFromCache(source.string()), nullptr);
}

TEST_F(AudioCacheTest, PruneReportsTheBytesItLeftBehind) {
    const auto source = writeSource("measured.wav", "measured-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x24)).isSuccess());

    auto preview = cache.prune(AudioCache::PrunePolicy{}, /*dryRun=*/true);
    ASSERT_TRUE(preview.isSuccess());
    const auto report = preview.getValue().value();

    // bytes_retained is the number that says whether the cache is a problem,
    // and it is what the size cap is measured against.
    EXPECT_GT(report.bytesRetained, 0U);
    EXPECT_EQ(report.bytesReclaimed, 0U);
}

TEST_F(AudioCacheTest, SizeCapEvictsLeastRecentlyReadFirst) {
    const auto oldest = writeSource("oldest.wav", "oldest-source");
    const auto middle = writeSource("middle.wav", "middle-source");
    const auto newest = writeSource("newest.wav", "newest-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, oldest, makeAudioData(0x25)).isSuccess());
    ASSERT_TRUE(save(cache, middle, makeAudioData(0x26)).isSuccess());
    ASSERT_TRUE(save(cache, newest, makeAudioData(0x27)).isSuccess());

    backdateLastRead(entryDirFor("oldest"), std::chrono::hours(96));
    backdateLastRead(entryDirFor("middle"), std::chrono::hours(48));

    const auto oldestDir = entryDirFor("oldest");
    ASSERT_FALSE(oldestDir.empty());

    auto measured = cache.prune(AudioCache::PrunePolicy{}, /*dryRun=*/true);
    ASSERT_TRUE(measured.isSuccess());
    const auto total = measured.getValue().value().bytesRetained;
    ASSERT_GT(total, 0U);

    // A ceiling that only two of the three entries can fit under.
    const auto ceiling = total * 2 / 3;
    auto applied = cache.prune(cappedAt(ceiling), /*dryRun=*/false);
    ASSERT_TRUE(applied.isSuccess());
    const auto report = applied.getValue().value();

    EXPECT_EQ(report.oversizeEntries, 1U);
    EXPECT_EQ(report.idleEntries, 0U) << "no idle age was configured";
    EXPECT_FALSE(fs::exists(oldestDir)) << "the least-recently-read entry is the one that should go";
    EXPECT_NE(cache.tryLoadFromCache(middle.string()), nullptr);
    EXPECT_NE(cache.tryLoadFromCache(newest.string()), nullptr);
    EXPECT_LE(report.bytesRetained, ceiling);
}

TEST_F(AudioCacheTest, SizeCapLeavesAnUnderBudgetCacheAlone) {
    const auto source = writeSource("roomy.wav", "roomy-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x28)).isSuccess());
    backdateLastRead(entryDirFor("roomy"), std::chrono::hours(24 * 500));

    auto applied = cache.prune(cappedAt(64ULL * 1024 * 1024 * 1024), /*dryRun=*/false);
    ASSERT_TRUE(applied.isSuccess());

    EXPECT_EQ(applied.getValue().value().oversizeEntries, 0U);
    EXPECT_NE(cache.tryLoadFromCache(source.string()), nullptr)
        << "age alone must not evict when only a size cap is configured";
}

TEST_F(AudioCacheTest, PolicyPruneDryRunDeletesNothingAndCreatesNothing) {
    // The idle and size tiers must honor the same strictly-read-only dry run
    // the unusable tiers do (issue #168): no deletions, no new .lock files,
    // and no last-read stamps rewritten.
    const auto source = writeSource("policypreview.wav", "policy-preview-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x29)).isSuccess());
    const auto entryDir = entryDirFor("policypreview");
    backdateLastRead(entryDir, std::chrono::hours(96));

    auto snapshot = [&] {
        std::vector<std::pair<std::string, fs::file_time_type>> state;
        for (const auto &entry : fs::recursive_directory_iterator(root_ / ".opus_cache")) {
            state.emplace_back(entry.path().string(), fs::last_write_time(entry.path()));
        }
        std::sort(state.begin(), state.end());
        return state;
    };

    const auto before = snapshot();
    AudioCache::PrunePolicy policy = idleAfter(std::chrono::hours(24));
    policy.maxTotalBytes = 1; // aggressive on both tiers
    auto preview = cache.prune(policy, /*dryRun=*/true);
    ASSERT_TRUE(preview.isSuccess());
    const auto after = snapshot();

    EXPECT_EQ(before, after) << "a dry run must not create, remove, or re-stamp any file";
    EXPECT_EQ(preview.getValue().value().idleEntries, 1U) << "but it must still report what it would evict";
    EXPECT_NE(cache.tryLoadFromCache(source.string()), nullptr);
}

TEST_F(AudioCacheTest, PruneAbortsBetweenEntriesWhenAskedToStop) {
    // Shutdown must not wait on a walk of a big or slow cache, so the sweep
    // checks for an abort between entries (issue #225).
    const auto first = writeSource("abort-one.wav", "one");
    const auto second = writeSource("abort-two.wav", "two");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, first, makeAudioData(0x2A)).isSuccess());
    ASSERT_TRUE(save(cache, second, makeAudioData(0x2B)).isSuccess());

    AudioCache::PrunePolicy policy = idleAfter(std::chrono::seconds(1));
    policy.abortRequested = [] { return true; };

    auto applied = cache.prune(policy, /*dryRun=*/false);
    ASSERT_TRUE(applied.isSuccess());
    const auto report = applied.getValue().value();

    EXPECT_TRUE(report.aborted);
    EXPECT_EQ(report.entriesScanned, 0U) << "the abort is checked before any entry is examined";
    EXPECT_NE(cache.tryLoadFromCache(first.string()), nullptr);
    EXPECT_NE(cache.tryLoadFromCache(second.string()), nullptr);
}

TEST_F(AudioCacheTest, IdleEvictionNeverTouchesALockedEntry) {
    // The guarantee that matters most for a live show: an entry another holder
    // has locked is SKIPPED, never deleted. Pulling cache files out from under
    // a session that is reading them is an audible mid-show failure, and the
    // only cost of backing off is that the next sweep tries again.
    const auto source = writeSource("locked.wav", "locked-source");
    AudioCache cache(root_.string());
    ASSERT_TRUE(save(cache, source, makeAudioData(0x2C)).isSuccess());

    const auto entryDir = entryDirFor("locked");
    ASSERT_FALSE(entryDir.empty());
    backdateLastRead(entryDir, std::chrono::hours(96));

    // flock is per open file description, so a second descriptor in this same
    // process conflicts exactly the way a peer process would.
    const auto lockPath = entryDir.string() + ".lock";
    const int heldFd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GE(heldFd, 0);
    ASSERT_EQ(::flock(heldFd, LOCK_EX | LOCK_NB), 0);

    auto applied = cache.prune(idleAfter(std::chrono::hours(24)), /*dryRun=*/false);

    ::flock(heldFd, LOCK_UN);
    ::close(heldFd);

    ASSERT_TRUE(applied.isSuccess());
    const auto report = applied.getValue().value();
    EXPECT_EQ(report.idleEntries, 0U) << "a locked entry must never be deleted";
    EXPECT_GE(report.skippedEntries, 1U);
    EXPECT_TRUE(fs::exists(entryDir));
    EXPECT_NE(cache.tryLoadFromCache(source.string()), nullptr);
}

TEST_F(AudioCacheTest, PolicyForHostIsTighterInTravelMode) {
    // The travel server is an 8 GB Pi with little disk headroom; the main
    // server has 64 GB and room to keep a season of material warm.
    ::unsetenv(AUDIO_CACHE_MAX_IDLE_DAYS_ENV);
    ::unsetenv(AUDIO_CACHE_MAX_BYTES_ENV);

    const auto main = AudioCache::PrunePolicy::forHost(/*travelMode=*/false);
    const auto travel = AudioCache::PrunePolicy::forHost(/*travelMode=*/true);

    ASSERT_TRUE(main.maxIdleAge.has_value());
    ASSERT_TRUE(travel.maxIdleAge.has_value());
    EXPECT_LT(travel.maxIdleAge->count(), main.maxIdleAge->count());
    EXPECT_LT(travel.maxTotalBytes, main.maxTotalBytes);
    EXPECT_GT(main.maxTotalBytes, 0U);
}

TEST_F(AudioCacheTest, PolicyForHostHonorsEnvironmentOverridesAndZeroDisables) {
    ::setenv(AUDIO_CACHE_MAX_IDLE_DAYS_ENV, "0", 1);
    ::setenv(AUDIO_CACHE_MAX_BYTES_ENV, "0", 1);
    const auto disabled = AudioCache::PrunePolicy::forHost(/*travelMode=*/false);
    EXPECT_FALSE(disabled.maxIdleAge.has_value()) << "0 days must disable idle eviction entirely";
    EXPECT_EQ(disabled.maxTotalBytes, 0U) << "0 bytes must mean unbounded, not evict-everything";

    ::setenv(AUDIO_CACHE_MAX_IDLE_DAYS_ENV, "7", 1);
    ::setenv(AUDIO_CACHE_MAX_BYTES_ENV, "1048576", 1);
    const auto overridden = AudioCache::PrunePolicy::forHost(/*travelMode=*/false);
    ASSERT_TRUE(overridden.maxIdleAge.has_value());
    EXPECT_EQ(overridden.maxIdleAge->count(), 7 * 24 * 3600);
    EXPECT_EQ(overridden.maxTotalBytes, 1048576U);

    ::unsetenv(AUDIO_CACHE_MAX_IDLE_DAYS_ENV);
    ::unsetenv(AUDIO_CACHE_MAX_BYTES_ENV);
}

// --- issue #235: where the cache lives ---

TEST_F(AudioCacheTest, DefaultsToTheHistoricalLocationInsideTheSoundLibrary) {
    AudioCache cache(root_.string());
    const auto where = fs::path(cache.cacheDirectory());

    EXPECT_EQ(where.parent_path().filename().string(), ".opus_cache");
    EXPECT_EQ(where.parent_path().parent_path(), root_) << "an unset override must change nothing";
}

TEST_F(AudioCacheTest, HonoursAConfiguredCacheRootOutsideTheSoundLibrary) {
    const auto elsewhere = fs::temp_directory_path() / ("audio-cache-elsewhere-" + std::to_string(::getpid()));
    fs::create_directories(elsewhere);

    AudioCache cache(root_.string(), elsewhere.string());
    const auto where = fs::path(cache.cacheDirectory());

    // <override>/<hostname> — the hostname scoping is kept, because two hosts
    // must never share entries.
    EXPECT_EQ(where.parent_path(), elsewhere);
    EXPECT_FALSE(where.filename().empty());

    // And it genuinely works from there.
    const auto source = writeSource("relocated.wav", "relocated-source");
    ASSERT_TRUE(save(cache, source, makeAudioData(0x51)).isSuccess());
    ASSERT_NE(cache.tryLoadFromCache(source.string()), nullptr);
    EXPECT_FALSE(fs::exists(root_ / ".opus_cache")) << "nothing should have been written to the sound library";

    std::error_code ec;
    fs::remove_all(elsewhere, ec);
}

TEST_F(AudioCacheTest, RefusesACacheRootThatWouldPutItBackInTheSoundWalk) {
    // The ONLY reason the default location is invisible to the sound-store
    // walkers is that they skip dot-directories. A configured root inside the
    // library without one would re-create issue #223 — 17 files per sound in
    // the path of every GET /api/v1/sound, a 50-66 s stall — by configuration.
    const auto unsafe = root_ / "packet-cache";

    AudioCache cache(root_.string(), unsafe.string());
    const auto where = fs::path(cache.cacheDirectory());

    EXPECT_EQ(where.parent_path().filename().string(), ".opus_cache")
        << "an unsafe override must fall back to the default, not be honoured";
    EXPECT_FALSE(fs::exists(unsafe)) << "and must not create the directory it refused";
}

TEST_F(AudioCacheTest, ADotPrefixedRootInsideTheSoundLibraryIsFine) {
    // Still inside the library, but the walkers skip it, so it is safe.
    const auto safe = root_ / ".packets";

    AudioCache cache(root_.string(), safe.string());

    EXPECT_EQ(fs::path(cache.cacheDirectory()).parent_path(), safe);
}

TEST_F(AudioCacheTest, AnUnusableConfiguredRootFallsBackRatherThanLosingTheCache) {
    // A mistyped setting should cost the faster location, not caching
    // altogether: main.cpp turns a constructor throw into "no cache at all".
    const auto unwritable = fs::temp_directory_path() / ("audio-cache-ro-" + std::to_string(::getpid()));
    fs::create_directories(unwritable);
    fs::permissions(unwritable, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);

    std::unique_ptr<AudioCache> cache;
    ASSERT_NO_THROW(cache = std::make_unique<AudioCache>(root_.string(), (unwritable / "nested").string()));

    fs::permissions(unwritable, fs::perms::owner_all, fs::perm_options::replace);

    ASSERT_NE(cache, nullptr);
    EXPECT_EQ(fs::path(cache->cacheDirectory()).parent_path().filename().string(), ".opus_cache")
        << "an unusable configured root must fall back to the working default";

    const auto source = writeSource("fallback.wav", "fallback-source");
    ASSERT_TRUE(save(*cache, source, makeAudioData(0x52)).isSuccess())
        << "and the cache must still be usable afterwards";

    // The resolved path is what callers and the startup log report, so it must
    // say the default — claiming the configured location while sitting in the
    // default one would send an operator looking in the wrong directory.
    EXPECT_EQ(fs::path(cache->cacheDirectory()).parent_path(), root_ / ".opus_cache");

    std::error_code ec;
    fs::remove_all(unwritable, ec);
}

} // namespace
} // namespace creatures::util
