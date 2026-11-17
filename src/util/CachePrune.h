#pragma once

//
// CachePrune.h — the policy half of pruning an on-disk cache.
//
// Extracted from AudioCache (issue #225) when the ad-hoc pipeline needed the
// same two tiers (issue #271). The *classification* half — what counts as an
// orphan, a crashed save, an abandoned temporary — is specific to each cache's
// on-disk layout and stays with that cache. What is shared is everything after
// it: given a list of entries that are usable and could be kept, decide which
// ones to discard because they are idle or because the cache is over budget.
//
// Each cache builds its own candidate list and supplies its own eviction
// callback, so the locking and safety rules stay where they belong.
//

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace creatures::util {

/**
 * @brief What a prune pass is allowed to throw away.
 */
struct CachePrunePolicy {
    /// Entries not used in longer than this are evicted. Unset = keep forever.
    std::optional<std::chrono::seconds> maxIdleAge{};

    /// Ceiling on the retained cache, in bytes. 0 = unbounded.
    std::uintmax_t maxTotalBytes{0};

    /// Nothing younger than this is evicted by either tier. Unset = no floor.
    ///
    /// The size tier sorts by age and otherwise has no notion of "too new to
    /// touch" — it is deliberately willing to ignore the idle cutoff, because
    /// that is the only way it can bound a cache that is entirely in rotation.
    /// For a cache whose entries are being WRITTEN as the sweep runs (the
    /// ad-hoc root, where a streaming session fills a directory over tens of
    /// seconds) that is not safe on its own, and this is the floor that makes
    /// it safe (issue #271).
    std::optional<std::chrono::seconds> minAge{};

    /// Checked between entries so a long sweep can be abandoned at
    /// shutdown instead of delaying it. Unset = run to completion.
    std::function<bool()> abortRequested{};

    /// Human-readable summary, for the startup log line.
    [[nodiscard]] std::string describe() const;
};

/**
 * @brief What a prune pass found and (unless dry-run) removed.
 *
 * Shared so both caches report the same shape; a cache that cannot produce a
 * given count just leaves it at zero.
 */
struct CachePruneReport {
    std::size_t entriesScanned{0};
    std::size_t orphanedEntries{0};   // source file no longer exists
    std::size_t incompleteEntries{0}; // no completion marker (crashed save)
    std::size_t temporaryFiles{0};    // abandoned *.tmp.<pid>.<n>
    std::size_t orphanedLockFiles{0}; // .lock whose cache directory is gone
    std::size_t idleEntries{0};       // usable, but unused past maxIdleAge
    std::size_t oversizeEntries{0};   // usable, evicted LRU to fit maxTotalBytes
    std::size_t skippedEntries{0};    // candidate that raced a read, or lock unavailable
    std::size_t protectedEntries{0};  // too young to evict (minAge)
    std::uintmax_t bytesReclaimed{0};
    std::uintmax_t bytesRetained{0}; // size of the cache the pass left behind
    bool dryRun{true};
    bool aborted{false}; // shutdown interrupted the sweep
    // Bounded sample of what was (or would be) removed, for the operator.
    std::vector<std::string> removed;
};

/**
 * @brief One entry the policy tiers may decide to discard.
 *
 * `lastUsed` means whatever "used" means for that cache: the Opus cache stamps
 * its completion marker on every read, while an ad-hoc render is only ever
 * written, so there it is the newest mtime in the entry.
 */
struct PruneCandidate {
    std::filesystem::path path;
    std::filesystem::file_time_type lastUsed;
    std::uintmax_t bytes{0};
    /// The scan already failed to take this entry's lock, so an eviction
    /// pass must not pay the acquisition timeout a second time for it.
    bool lockUnavailable{false};
};

/**
 * @brief Evict one candidate. Returns false when the entry was left alone.
 *
 * Declining is always the safe answer: the only cost is that the next sweep
 * retries. The callback owns the locking, the re-check against a concurrent
 * read, the actual removal, and the `bytesReclaimed` / `bytesRetained`
 * bookkeeping for the entry it removed.
 */
using PruneEvictFn = std::function<bool(const PruneCandidate &, const char *reason)>;

/**
 * @brief Apply the idle tier, then the size tier, in that order.
 *
 * `candidates` is consumed: entries the idle tier evicted are removed from it,
 * so a caller that wants to keep inspecting survivors can.
 *
 * `subject` names the cache in log lines ("Audio cache", "Ad-hoc store").
 */
void applyPrunePolicyTiers(const CachePrunePolicy &policy, std::vector<PruneCandidate> &candidates,
                           CachePruneReport &report, const PruneEvictFn &evict, const char *subject);

/**
 * @brief Best-effort recursive size of a directory: the sum of its files.
 */
[[nodiscard]] std::uintmax_t directorySizeBytes(const std::filesystem::path &dir);

/**
 * @brief The newest mtime of anything inside `dir` (or the directory's own).
 *
 * The right clock for an entry nobody reads but something may still be
 * writing: a session directory's own mtime only moves when a file is created
 * in it, so a long render whose last file was added ten minutes ago reads as
 * ten minutes idle no matter how much has been appended since.
 */
[[nodiscard]] std::optional<std::filesystem::file_time_type> newestWriteTime(const std::filesystem::path &dir);

} // namespace creatures::util
