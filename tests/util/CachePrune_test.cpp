//
// Tests for the shared prune tiers (issues #225, #271).
//
// These exercise `applyPrunePolicyTiers` on its own: the tiers are pure
// decision-making over a candidate list, so nothing here touches a disk. The
// two caches' candidate-building and eviction live in their own tests.
//

#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "util/CachePrune.h"

namespace creatures::util {

namespace {

using Clock = std::filesystem::file_time_type::clock;

PruneCandidate candidate(const std::string &name, std::chrono::seconds age, std::uintmax_t bytes) {
    return PruneCandidate{std::filesystem::path("/fake") / name, Clock::now() - age, bytes, false};
}

/// Records what the tiers asked to remove, and says yes to all of it.
struct Recorder {
    std::vector<std::string> evicted;
    std::vector<std::string> reasons;
    CachePruneReport *report{nullptr};

    PruneEvictFn fn() {
        return [this](const PruneCandidate &target, const char *reason) {
            evicted.push_back(target.path.filename().string());
            reasons.emplace_back(reason);
            // The real callbacks own this bookkeeping, so the fake must too or
            // the size tier never sees itself making progress.
            report->bytesReclaimed += target.bytes;
            report->bytesRetained -= std::min(report->bytesRetained, target.bytes);
            return true;
        };
    }
};

std::uintmax_t totalBytes(const std::vector<PruneCandidate> &candidates) {
    std::uintmax_t bytes = 0;
    for (const auto &c : candidates) {
        bytes += c.bytes;
    }
    return bytes;
}

} // namespace

TEST(CachePruneTiersTest, DefaultPolicyEvictsNothing) {
    std::vector<PruneCandidate> candidates{candidate("ancient", std::chrono::hours(24 * 365), 1000),
                                           candidate("fresh", std::chrono::seconds(1), 1000)};
    CachePruneReport report;
    report.bytesRetained = totalBytes(candidates);
    Recorder recorder;
    recorder.report = &report;

    applyPrunePolicyTiers(CachePrunePolicy{}, candidates, report, recorder.fn(), "Test cache");

    // A default-constructed policy is the historical orphan-only behavior:
    // neither tier that discards usable entries is enabled.
    EXPECT_TRUE(recorder.evicted.empty());
    EXPECT_EQ(report.idleEntries, 0u);
    EXPECT_EQ(report.oversizeEntries, 0u);
}

TEST(CachePruneTiersTest, IdleTierEvictsOnlyEntriesPastTheCutoff) {
    std::vector<PruneCandidate> candidates{candidate("stale", std::chrono::hours(48), 1000),
                                           candidate("recent", std::chrono::hours(2), 1000)};
    CachePruneReport report;
    report.bytesRetained = totalBytes(candidates);
    Recorder recorder;
    recorder.report = &report;

    CachePrunePolicy policy;
    policy.maxIdleAge = std::chrono::hours(12);

    applyPrunePolicyTiers(policy, candidates, report, recorder.fn(), "Test cache");

    ASSERT_EQ(recorder.evicted.size(), 1u);
    EXPECT_EQ(recorder.evicted.front(), "stale");
    EXPECT_EQ(recorder.reasons.front(), "idle");
    EXPECT_EQ(report.idleEntries, 1u);
    EXPECT_EQ(report.bytesReclaimed, 1000u);
    EXPECT_EQ(report.bytesRetained, 1000u);

    // Survivors are left in the list for the size tier (and for a caller that
    // wants to keep inspecting them).
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates.front().path.filename().string(), "recent");
}

TEST(CachePruneTiersTest, SizeTierEvictsLeastRecentlyUsedUntilUnderTheCap) {
    // 4 x 1000 bytes, oldest first. A 2500 byte cap has to drop two of them.
    std::vector<PruneCandidate> candidates{
        candidate("oldest", std::chrono::hours(40), 1000), candidate("older", std::chrono::hours(30), 1000),
        candidate("newer", std::chrono::hours(20), 1000), candidate("newest", std::chrono::hours(10), 1000)};
    CachePruneReport report;
    report.bytesRetained = totalBytes(candidates);
    Recorder recorder;
    recorder.report = &report;

    CachePrunePolicy policy;
    policy.maxTotalBytes = 2500;

    applyPrunePolicyTiers(policy, candidates, report, recorder.fn(), "Test cache");

    ASSERT_EQ(recorder.evicted.size(), 2u);
    EXPECT_EQ(recorder.evicted[0], "oldest");
    EXPECT_EQ(recorder.evicted[1], "older");
    EXPECT_EQ(report.oversizeEntries, 2u);
    EXPECT_EQ(report.bytesRetained, 2000u);
    EXPECT_LE(report.bytesRetained, policy.maxTotalBytes);
}

TEST(CachePruneTiersTest, SizeTierDoesNothingWhenAlreadyUnderTheCap) {
    std::vector<PruneCandidate> candidates{candidate("a", std::chrono::hours(100), 1000)};
    CachePruneReport report;
    report.bytesRetained = totalBytes(candidates);
    Recorder recorder;
    recorder.report = &report;

    CachePrunePolicy policy;
    policy.maxTotalBytes = 10000;

    applyPrunePolicyTiers(policy, candidates, report, recorder.fn(), "Test cache");

    EXPECT_TRUE(recorder.evicted.empty());
}

TEST(CachePruneTiersTest, MinAgeProtectsAYoungEntryFromTheSizeTier) {
    // This is the guard that makes the size cap safe for the ad-hoc root
    // (issue #271): the cap deliberately ignores the idle cutoff, so without a
    // floor it is free to delete the directory a live session is writing into.
    std::vector<PruneCandidate> candidates{candidate("being-written", std::chrono::seconds(30), 5000),
                                           candidate("old", std::chrono::hours(40), 1000)};
    CachePruneReport report;
    report.bytesRetained = totalBytes(candidates);
    Recorder recorder;
    recorder.report = &report;

    CachePrunePolicy policy;
    policy.maxTotalBytes = 100; // far too small for either entry
    policy.minAge = std::chrono::minutes(15);

    applyPrunePolicyTiers(policy, candidates, report, recorder.fn(), "Test cache");

    ASSERT_EQ(recorder.evicted.size(), 1u);
    EXPECT_EQ(recorder.evicted.front(), "old");
    EXPECT_EQ(report.protectedEntries, 1u);

    // Still over the cap, and that is the correct answer: the only thing left
    // is too new to touch.
    EXPECT_GT(report.bytesRetained, policy.maxTotalBytes);
}

TEST(CachePruneTiersTest, MinAgeProtectsAYoungEntryFromTheIdleTier) {
    // Only reachable by misconfiguration (minAge > maxIdleAge), but it should
    // keep the file rather than split the difference.
    std::vector<PruneCandidate> candidates{candidate("young", std::chrono::minutes(2), 1000)};
    CachePruneReport report;
    report.bytesRetained = totalBytes(candidates);
    Recorder recorder;
    recorder.report = &report;

    CachePrunePolicy policy;
    policy.maxIdleAge = std::chrono::minutes(1);
    policy.minAge = std::chrono::minutes(15);

    applyPrunePolicyTiers(policy, candidates, report, recorder.fn(), "Test cache");

    EXPECT_TRUE(recorder.evicted.empty());
    EXPECT_EQ(report.protectedEntries, 1u);
    EXPECT_EQ(candidates.size(), 1u);
}

TEST(CachePruneTiersTest, AnAbortedScanSkipsBothTiers) {
    // A sweep abandoned at shutdown must not then go on to evict: the scan it
    // based its candidate list on is incomplete.
    std::vector<PruneCandidate> candidates{candidate("ancient", std::chrono::hours(24 * 365), 100000)};
    CachePruneReport report;
    report.aborted = true;
    report.bytesRetained = totalBytes(candidates);
    Recorder recorder;
    recorder.report = &report;

    CachePrunePolicy policy;
    policy.maxIdleAge = std::chrono::hours(1);
    policy.maxTotalBytes = 1;

    applyPrunePolicyTiers(policy, candidates, report, recorder.fn(), "Test cache");

    EXPECT_TRUE(recorder.evicted.empty());
}

TEST(CachePruneTiersTest, DecliningAnEvictionKeepsTheEntry) {
    std::vector<PruneCandidate> candidates{candidate("locked", std::chrono::hours(40), 1000),
                                           candidate("free", std::chrono::hours(40), 1000)};
    CachePruneReport report;
    report.bytesRetained = totalBytes(candidates);

    // Says no to one entry, the way a real callback does when the entry was
    // touched since it was sampled.
    PruneEvictFn evict = [&report](const PruneCandidate &target, const char *) {
        if (target.path.filename() == "locked") {
            ++report.skippedEntries;
            return false;
        }
        report.bytesReclaimed += target.bytes;
        report.bytesRetained -= std::min(report.bytesRetained, target.bytes);
        return true;
    };

    CachePrunePolicy policy;
    policy.maxIdleAge = std::chrono::hours(12);

    applyPrunePolicyTiers(policy, candidates, report, evict, "Test cache");

    EXPECT_EQ(report.idleEntries, 1u);
    EXPECT_EQ(report.skippedEntries, 1u);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates.front().path.filename().string(), "locked");
}

TEST(CachePrunePolicyTest, DescribeMentionsBothTiersAndTheFloor) {
    CachePrunePolicy policy;
    EXPECT_NE(policy.describe().find("disabled"), std::string::npos);
    EXPECT_NE(policy.describe().find("unbounded"), std::string::npos);

    policy.maxIdleAge = std::chrono::hours(48);
    policy.maxTotalBytes = 16ULL * 1024 * 1024 * 1024;
    policy.minAge = std::chrono::minutes(15);
    const auto described = policy.describe();
    EXPECT_NE(described.find("2 day(s)"), std::string::npos);
    EXPECT_NE(described.find("16384 MB"), std::string::npos);
    EXPECT_NE(described.find("900"), std::string::npos);
}

} // namespace creatures::util
