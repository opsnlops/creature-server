//
// Tests for the ad-hoc store's sweeper (issue #271).
//
// The fixture builds the tree the pipeline actually writes, with a mix of ages,
// and each test pins down one of the four defects the old startup-only sweep
// had. The two that matter most:
//
//   * a loose `dialog_*.wav` in the root was never a candidate, because the old
//     sweep skipped everything that was not a directory;
//   * nothing inside `preview-exports/` or `dialog-cache/` was ever a
//     candidate, because the old sweep dated each tree by the tree's own mtime
//     and those two are written to constantly.
//

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/config/Configuration.h"
#include "server/storage/AdHocStore.h"
#include "server/storage/Storage.h"

namespace creatures {
extern std::shared_ptr<Configuration> config;
} // namespace creatures

namespace creatures::storage {

namespace {

namespace fs = std::filesystem;
using Clock = fs::file_time_type::clock;

/// Exposes the protected setter, the way Storage_test does.
class TestConfiguration : public Configuration {
  public:
    using Configuration::setAdHocCacheDir;
    using Configuration::setSoundFileLocation;
};

class AdHocStoreTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cacheRoot_ = fs::temp_directory_path() /
                     ("adhoc-store-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                      ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(cacheRoot_, ec);
        fs::create_directories(cacheRoot_);

        savedConfig_ = creatures::config;
        auto testConfig = std::make_shared<TestConfiguration>();
        testConfig->setAdHocCacheDir(cacheRoot_.string());
        testConfig->setSoundFileLocation((cacheRoot_ / "permanent").string());
        creatures::config = testConfig;

        adHoc_ = requireRoot(Persistence::AdHoc);
        generationCache_ = requireRoot(Persistence::GenerationCache);
        scratch_ = requireRoot(Persistence::JobScratch);
    }

    void TearDown() override {
        creatures::config = savedConfig_;
        std::error_code ec;
        fs::remove_all(cacheRoot_, ec);
    }

    static fs::path requireRoot(Persistence persistence) {
        auto result = root(persistence);
        EXPECT_TRUE(result.isSuccess());
        return result.isSuccess() ? result.getValue().value() : fs::path{};
    }

    /// A file of `bytes` bytes, aged `age` old.
    static void writeAged(const fs::path &path, std::size_t bytes, std::chrono::seconds age) {
        fs::create_directories(path.parent_path());
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << std::string(bytes, 'x');
        }
        std::error_code ec;
        fs::last_write_time(path, Clock::now() - age, ec);
        EXPECT_FALSE(ec) << "could not age " << path.string() << ": " << ec.message();
    }

    /// A directory holding one aged file, with the directory's own mtime aged
    /// to match. Both matter: `newestWriteTime` takes the maximum, so a
    /// directory left with a fresh mtime reads as fresh however old its
    /// contents are.
    static void writeAgedDir(const fs::path &dir, std::size_t bytes, std::chrono::seconds age) {
        writeAged(dir / "s1.wav", bytes, age);
        std::error_code ec;
        fs::last_write_time(dir, Clock::now() - age, ec);
    }

    static bool exists(const fs::path &path) {
        std::error_code ec;
        return fs::exists(path, ec) && !ec;
    }

    /// A 12 hour idle tier and no size cap: the production idle policy.
    static creatures::util::CachePrunePolicy idlePolicy() {
        creatures::util::CachePrunePolicy policy;
        policy.maxIdleAge = std::chrono::hours(12);
        policy.minAge = std::chrono::minutes(15);
        return policy;
    }

    fs::path cacheRoot_;
    fs::path adHoc_;
    fs::path generationCache_;
    fs::path scratch_;
    std::shared_ptr<Configuration> savedConfig_;
};

constexpr auto kStale = std::chrono::hours(48);
constexpr auto kFresh = std::chrono::seconds(5);

} // namespace

// --- Defect 1: the buckets live where the configuration says ----------------

TEST_F(AdHocStoreTest, TheThreeBucketsLiveUnderTheConfiguredRoot) {
    EXPECT_EQ(adHoc_, cacheRoot_ / "adhoc");
    EXPECT_EQ(generationCache_, cacheRoot_ / "dialog-cache");
    EXPECT_EQ(scratch_, cacheRoot_ / "scratch");

    // Siblings now, not nested. The generation cache used to live INSIDE the
    // ad-hoc directory, which is how one stale-looking parent could take the
    // whole cache with it.
    EXPECT_EQ(generationCache_.parent_path(), adHoc_.parent_path());
}

// --- Defect 2: a loose file in the root -------------------------------------

TEST_F(AdHocStoreTest, ReclaimsAStaleLooseDialogWav) {
    // This is the file the old sweep could never touch: JobWorker writes ad-hoc
    // dialog audio as a loose `dialog_<name>.wav` in the root, and the old
    // sweep skipped anything that was not a directory.
    const auto stale = adHoc_ / "dialog_old-scene.wav";
    const auto fresh = adHoc_ / "dialog_new-scene.wav";
    writeAged(stale, 2048, kStale);
    writeAged(fresh, 2048, kFresh);

    auto result = AdHocStore::prune(idlePolicy(), /*dryRun=*/false);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_FALSE(exists(stale));
    EXPECT_TRUE(exists(fresh));
    EXPECT_GE(result.getValue()->idleEntries, 1u);
    EXPECT_GE(result.getValue()->bytesReclaimed, 2048u);
}

TEST_F(AdHocStoreTest, ADryRunRemovesNothingButStillMeasures) {
    const auto stale = adHoc_ / "dialog_old-scene.wav";
    writeAged(stale, 4096, kStale);

    auto result = AdHocStore::prune(idlePolicy(), /*dryRun=*/true);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_TRUE(exists(stale));
    EXPECT_TRUE(result.getValue()->dryRun);
    EXPECT_GE(result.getValue()->bytesReclaimed, 4096u);
}

// --- Defect 3: a tree judged by its own mtime -------------------------------

TEST_F(AdHocStoreTest, AgesPreviewExportsFileByFileAndKeepsTheContainer) {
    // `preview-exports/` collects every 17-channel voice take. Its mtime moves
    // on every new take, so under the old rule it never aged past the cutoff
    // and nothing inside it was ever considered.
    const auto container = adHoc_ / "preview-exports";
    const auto staleTake = container / "dialog-17ch-old.wav";
    const auto freshTake = container / "dialog-17ch-new.wav";
    writeAged(staleTake, 1024, kStale);
    writeAged(freshTake, 1024, kFresh);
    // Exactly the trap: the container looks brand new.
    std::error_code ec;
    fs::last_write_time(container, Clock::now(), ec);

    auto result = AdHocStore::prune(idlePolicy(), /*dryRun=*/false);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_FALSE(exists(staleTake)) << "a stale take inside a fresh container must still be reclaimed";
    EXPECT_TRUE(exists(freshTake));
    EXPECT_TRUE(exists(container)) << "the container itself must never be a candidate";
}

TEST_F(AdHocStoreTest, AgesTheGenerationCacheByKeyAndKeepsItsRoot) {
    const auto staleKey = generationCache_ / "key-old";
    const auto freshKey = generationCache_ / "key-new";
    writeAgedDir(staleKey, 1024, kStale);
    writeAgedDir(freshKey, 1024, kFresh);
    std::error_code ec;
    fs::last_write_time(generationCache_, Clock::now(), ec);

    auto result = AdHocStore::prune(idlePolicy(), /*dryRun=*/false);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_FALSE(exists(staleKey));
    EXPECT_TRUE(exists(freshKey));
    EXPECT_TRUE(exists(generationCache_)) << "an idle day must not take the whole generation cache";
}

TEST_F(AdHocStoreTest, DatesASessionDirectoryByItsNewestFile) {
    // A streaming session writes one WAV per sentence over tens of seconds. The
    // directory's own mtime only moves when a file is created in it, so dating
    // the entry by the newest file inside is what keeps a long render safe.
    const auto session = adHoc_ / "11111111-1111-4111-8111-111111111111";
    writeAged(session / "s1.wav", 1024, kStale);
    writeAged(session / "s2.wav", 1024, kFresh);
    std::error_code ec;
    fs::last_write_time(session, Clock::now() - kStale, ec);

    auto result = AdHocStore::prune(idlePolicy(), /*dryRun=*/false);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_TRUE(exists(session)) << "a session with a fresh sentence in it is not idle";
    EXPECT_TRUE(exists(session / "s1.wav"));
}

TEST_F(AdHocStoreTest, ReclaimsAStaleSessionDirectoryAndCrashedJobScratch) {
    const auto staleSession = adHoc_ / "22222222-2222-4222-8222-222222222222";
    const auto staleScratch = scratch_ / "33333333-3333-4333-8333-333333333333";
    writeAgedDir(staleSession, 1024, kStale);
    writeAgedDir(staleScratch, 1024, kStale);

    auto result = AdHocStore::prune(idlePolicy(), /*dryRun=*/false);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_FALSE(exists(staleSession));
    EXPECT_FALSE(exists(staleScratch)) << "TempDirGuard missed it, so the sweep is the only thing that will";
}

// --- Defect 4: no ceiling ---------------------------------------------------

TEST_F(AdHocStoreTest, TheSizeCapEvictsLeastRecentlyWrittenFirst) {
    // All three are inside the idle window, so only the cap can reclaim them.
    writeAged(adHoc_ / "dialog_a.wav", 4096, std::chrono::hours(6));
    writeAged(adHoc_ / "dialog_b.wav", 4096, std::chrono::hours(4));
    writeAged(adHoc_ / "dialog_c.wav", 4096, std::chrono::hours(2));

    creatures::util::CachePrunePolicy policy;
    policy.maxIdleAge = std::chrono::hours(12);
    policy.minAge = std::chrono::minutes(15);
    policy.maxTotalBytes = 9000; // room for two of the three

    auto result = AdHocStore::prune(policy, /*dryRun=*/false);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_FALSE(exists(adHoc_ / "dialog_a.wav")) << "the oldest should go first";
    EXPECT_TRUE(exists(adHoc_ / "dialog_c.wav"));
    EXPECT_LE(result.getValue()->bytesRetained, policy.maxTotalBytes);
    EXPECT_GE(result.getValue()->oversizeEntries, 1u);
}

TEST_F(AdHocStoreTest, TheSizeCapWillNotTouchSomethingBeingWrittenRightNow) {
    // A live session, well under the floor, and a cap far too small for it.
    const auto live = adHoc_ / "44444444-4444-4444-8444-444444444444";
    writeAgedDir(live, 8192, std::chrono::seconds(2));

    creatures::util::CachePrunePolicy policy;
    policy.maxTotalBytes = 10;
    policy.minAge = std::chrono::minutes(15);

    auto result = AdHocStore::prune(policy, /*dryRun=*/false);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_TRUE(exists(live));
    EXPECT_GE(result.getValue()->protectedEntries, 1u);
}

// --- Housekeeping -----------------------------------------------------------

TEST_F(AdHocStoreTest, AbandonedTemporariesGoRegardlessOfAge) {
    // Nothing can ever read a half-written temporary, so there is no policy to
    // apply to it — including the minAge floor.
    const auto leftover = adHoc_ / "dialog_scene.wav.tmp";
    writeAged(leftover, 512, kFresh);

    auto result = AdHocStore::prune(idlePolicy(), /*dryRun=*/false);
    ASSERT_TRUE(result.isSuccess());

    EXPECT_FALSE(exists(leftover));
    EXPECT_GE(result.getValue()->temporaryFiles, 1u);
}

TEST_F(AdHocStoreTest, AnEmptyRootIsNotAnError) {
    auto result = AdHocStore::prune(idlePolicy(), /*dryRun=*/false);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.getValue()->entriesScanned, 0u);
    EXPECT_EQ(result.getValue()->bytesReclaimed, 0u);
}

TEST_F(AdHocStoreTest, ReportsWhatItLeftBehind) {
    writeAged(adHoc_ / "dialog_kept.wav", 3000, kFresh);
    writeAged(adHoc_ / "dialog_gone.wav", 1000, kStale);

    auto result = AdHocStore::prune(idlePolicy(), /*dryRun=*/false);
    ASSERT_TRUE(result.isSuccess());

    // What the counters gauge publishes, so it has to be the surviving set.
    EXPECT_EQ(result.getValue()->bytesRetained, 3000u);
    EXPECT_EQ(result.getValue()->bytesReclaimed, 1000u);
    EXPECT_EQ(result.getValue()->entriesScanned, 2u);
}

// --- The policy ------------------------------------------------------------

TEST_F(AdHocStoreTest, ThePolicyTakesItsIdleAgeFromTheAnimationTtl) {
    const auto policy = AdHocStore::prunePolicyForHost(/*travelMode=*/false, /*ttlHours=*/12);
    ASSERT_TRUE(policy.maxIdleAge.has_value());
    EXPECT_EQ(policy.maxIdleAge->count(), 12 * 3600);
    EXPECT_GT(policy.maxTotalBytes, 0u);
    ASSERT_TRUE(policy.minAge.has_value());
    EXPECT_EQ(policy.minAge->count(), ADHOC_CACHE_MIN_AGE_SECONDS);
}

TEST_F(AdHocStoreTest, TravelModeGetsASmallerCeiling) {
    const auto main = AdHocStore::prunePolicyForHost(/*travelMode=*/false, 12);
    const auto travel = AdHocStore::prunePolicyForHost(/*travelMode=*/true, 12);
    EXPECT_LT(travel.maxTotalBytes, main.maxTotalBytes) << "the travel rig is an 8 GB Pi";
}

TEST_F(AdHocStoreTest, AZeroTtlDisablesTheIdleTierRatherThanExpiringEverything) {
    const auto policy = AdHocStore::prunePolicyForHost(/*travelMode=*/false, /*ttlHours=*/0);
    EXPECT_FALSE(policy.maxIdleAge.has_value());
}

// --- Legacy paths ----------------------------------------------------------

TEST_F(AdHocStoreTest, ResolvesAPreMovePathUnderTheNewRoot) {
    // An ad-hoc sound_file is stored absolute, so rows written before the move
    // point into /tmp/creature-adhoc/... The file is under the new root now.
    const auto session = "55555555-5555-4555-8555-555555555555";
    writeAged(adHoc_ / session / "s1.wav", 128, kFresh);

    const std::string legacy = std::string("/tmp/creature-adhoc/") + session + "/s1.wav";
    const auto resolved = resolveSoundPath(legacy);

    EXPECT_EQ(resolved, adHoc_ / session / "s1.wav");
}

TEST_F(AdHocStoreTest, LeavesAnAbsolutePathAloneWhenItStillExists) {
    const auto real = adHoc_ / "dialog_here.wav";
    writeAged(real, 128, kFresh);
    EXPECT_EQ(resolveSoundPath(real.string()), real);
}

TEST_F(AdHocStoreTest, LeavesAnUnrelatedMissingPathAlone) {
    const std::string unrelated = "/mnt/creatures/sounds/dialog/nope.wav";
    EXPECT_EQ(resolveSoundPath(unrelated).string(), unrelated);
}

} // namespace creatures::storage
