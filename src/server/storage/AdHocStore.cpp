//
// AdHocStore.cpp — bounds the ad-hoc pipeline's working files (issue #271)
//

#include "server/storage/AdHocStore.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "server/config.h"
#include "server/namespace-stuffs.h"
#include "server/storage/Storage.h"
#include "util/environment.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::storage {

namespace {

namespace fs = std::filesystem;
using creatures::util::CachePruneReport;
using creatures::util::PruneCandidate;

/// One directory to sweep, and which of its children are containers rather
/// than entries.
///
/// A **container** is a long-lived directory the pipeline keeps writing into —
/// `preview-exports/` holds every 17-channel voice take, `lipsync-uploads/`
/// every uploaded WAV. It must never be a candidate itself (deleting it would
/// take the lot), and its mtime says nothing about how old its contents are.
/// Its children are the candidates. This distinction is the whole of defect 3.
struct SweepRoot {
    fs::path path;
    std::vector<std::string> containerDirs;
    const char *label;
};

bool isContainer(const SweepRoot &root, const std::string &name) {
    return std::find(root.containerDirs.begin(), root.containerDirs.end(), name) != root.containerDirs.end();
}

/// Reclaim an abandoned `.tmp` file wherever it turns up. Same unconditional
/// tier the Opus cache has: nothing can ever read a half-written temporary,
/// so there is no policy to apply to it.
void reclaimTemporary(const fs::path &path, bool dryRun, CachePruneReport &report,
                      const std::function<void(const std::string &)> &note) {
    std::error_code ec;
    const auto bytes = fs::is_directory(path, ec) ? creatures::util::directorySizeBytes(path) : fs::file_size(path, ec);
    ++report.temporaryFiles;
    report.bytesReclaimed += ec ? 0 : bytes;
    note(path.string());
    if (!dryRun) {
        std::error_code removeEc;
        fs::remove_all(path, removeEc);
    }
}

/// Build a candidate for one filesystem entry: a loose file, a per-run
/// directory, or one child of a container.
std::optional<PruneCandidate> candidateFor(const fs::path &path) {
    std::error_code ec;
    const bool directory = fs::is_directory(path, ec);
    if (ec) {
        return std::nullopt;
    }

    if (directory) {
        // Dated by the NEWEST file inside, not by the directory's own mtime: a
        // directory's mtime only moves when an entry is created in it, so a
        // streaming session that has been appending to the same files for
        // twenty minutes would otherwise read as twenty minutes idle.
        auto newest = creatures::util::newestWriteTime(path);
        if (!newest) {
            return std::nullopt;
        }
        return PruneCandidate{path, *newest, creatures::util::directorySizeBytes(path), false};
    }

    const auto when = fs::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto bytes = fs::file_size(path, ec);
    return PruneCandidate{path, when, ec ? 0 : bytes, false};
}

} // namespace

creatures::util::CachePrunePolicy AdHocStore::prunePolicyForHost(bool travelMode, uint32_t ttlHours) {
    creatures::util::CachePrunePolicy policy;

    if (ttlHours > 0) {
        policy.maxIdleAge = std::chrono::seconds(static_cast<std::chrono::seconds::rep>(ttlHours) * 3600LL);
    }

    policy.maxTotalBytes = static_cast<std::uintmax_t>(creatures::environmentToUnsignedLongLong(
        ADHOC_CACHE_MAX_BYTES_ENV, travelMode ? DEFAULT_ADHOC_CACHE_MAX_BYTES_TRAVEL : DEFAULT_ADHOC_CACHE_MAX_BYTES));

    policy.minAge = std::chrono::seconds(ADHOC_CACHE_MIN_AGE_SECONDS);

    return policy;
}

Result<CachePruneReport> AdHocStore::prune(const creatures::util::CachePrunePolicy &policy, bool dryRun,
                                           std::shared_ptr<OperationSpan> parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for AdHocStore.prune, creating a root span");
    }
    auto span = creatures::observability ? creatures::observability->createOperationSpan("AdHocStore.prune", parentSpan)
                                         : nullptr;
    if (span) {
        span->setAttribute("cache.prune.dry_run", dryRun);
        span->setAttribute("cache.prune.max_idle_hours",
                           static_cast<int64_t>(policy.maxIdleAge ? policy.maxIdleAge->count() / 3600 : 0));
        span->setAttribute("cache.prune.max_total_bytes", static_cast<int64_t>(policy.maxTotalBytes));
    }

    CachePruneReport report;
    report.dryRun = dryRun;
    constexpr std::size_t MAX_REPORTED = 100;

    auto note = [&report](const std::string &path) {
        if (report.removed.size() < MAX_REPORTED) {
            report.removed.push_back(path);
        }
    };

    // All three buckets. They were nested under one directory before #271
    // (`dialog-cache` lived inside `creature-adhoc`), which is part of how the
    // old sweep could delete the whole generation cache after an idle day;
    // now they are siblings and each is swept on its own terms.
    std::vector<SweepRoot> roots;
    struct BucketSpec {
        Persistence persistence;
        std::vector<std::string> containers;
        const char *label;
    };
    const std::array<BucketSpec, 3> buckets{{
        // `preview-exports/` holds the 17-channel voice takes, one file each.
        {Persistence::AdHoc, {"preview-exports"}, "adhoc"},
        // The generation cache's children ARE its entries (one per cache key).
        {Persistence::GenerationCache, {}, "dialog-cache"},
        // `lipsync-uploads/` holds one directory per upload; the rest are
        // per-job scratch dirs TempDirGuard normally removes, so anything left
        // here is from a job that crashed.
        {Persistence::JobScratch, {"lipsync-uploads"}, "scratch"},
    }};

    for (const auto &bucket : buckets) {
        auto rootResult = root(bucket.persistence);
        if (!rootResult.isSuccess()) {
            warn("AdHocStore.prune cannot reach the {} bucket: {}", bucket.label, rootResult.getError()->getMessage());
            continue;
        }
        roots.push_back(SweepRoot{rootResult.getValue().value(), bucket.containers, bucket.label});
    }

    // Deleting one entry. No lock files here: a single server process owns
    // these roots, unlike the Opus cache which can be shared between hosts.
    // What IS needed is the same re-check the Opus cache does — the age was
    // sampled before sorting, and a session may have written to this entry
    // since. Declining is always safe; the next sweep retries.
    creatures::util::PruneEvictFn evict = [&](const PruneCandidate &target, const char *reason) -> bool {
        if (!dryRun) {
            std::error_code ec;
            const bool directory = fs::is_directory(target.path, ec);
            const auto fresh =
                directory ? creatures::util::newestWriteTime(target.path) : [&]() -> std::optional<fs::file_time_type> {
                std::error_code freshEc;
                const auto when = fs::last_write_time(target.path, freshEc);
                if (freshEc) {
                    return std::nullopt;
                }
                return when;
            }();

            if (!fresh || *fresh > target.lastUsed) {
                ++report.skippedEntries;
                debug("Ad-hoc prune skipping {}: written since it was sampled", target.path.string());
                return false;
            }

            std::error_code removeEc;
            fs::remove_all(target.path, removeEc);
            if (removeEc) {
                ++report.skippedEntries;
                warn("Ad-hoc prune could not remove {}: {}", target.path.string(), removeEc.message());
                return false;
            }
        }

        report.bytesReclaimed += target.bytes;
        report.bytesRetained -= std::min(report.bytesRetained, target.bytes);
        note(fmt::format("{} ({})", target.path.string(), reason));
        return true;
    };

    std::vector<PruneCandidate> candidates;

    for (const auto &sweepRoot : roots) {
        if (report.aborted) {
            break;
        }

        std::error_code ec;
        if (!fs::exists(sweepRoot.path, ec) || ec) {
            continue;
        }

        // Snapshot the listing before touching anything: removing entries
        // while iterating the same directory is not something the standard
        // promises anything about.
        std::vector<fs::path> entries;
        for (const auto &entry : fs::directory_iterator(sweepRoot.path, ec)) {
            if (ec) {
                warn("AdHocStore.prune cannot iterate {}: {}", sweepRoot.path.string(), ec.message());
                break;
            }
            entries.push_back(entry.path());
        }

        for (const auto &path : entries) {
            // Shutdown must not wait on a walk of a big root, so a sweep can
            // be abandoned between entries. Everything done so far is durable.
            if (policy.abortRequested && policy.abortRequested()) {
                report.aborted = true;
                break;
            }

            const auto name = path.filename().string();

            // Our own write probe, and any abandoned atomic-write temporary.
            if (name.find(".tmp") != std::string::npos || name.rfind(".write-probe.", 0) == 0) {
                reclaimTemporary(path, dryRun, report, note);
                continue;
            }

            if (isContainer(sweepRoot, name)) {
                // Its children are the entries. One level only: a voice take
                // is a file, an upload is a directory, and neither nests.
                std::vector<fs::path> inner;
                std::error_code innerEc;
                for (const auto &child : fs::directory_iterator(path, innerEc)) {
                    if (innerEc) {
                        break;
                    }
                    inner.push_back(child.path());
                }
                for (const auto &child : inner) {
                    if (child.filename().string().find(".tmp") != std::string::npos) {
                        reclaimTemporary(child, dryRun, report, note);
                        continue;
                    }
                    ++report.entriesScanned;
                    if (auto candidate = candidateFor(child)) {
                        report.bytesRetained += candidate->bytes;
                        candidates.push_back(*candidate);
                    } else {
                        ++report.skippedEntries;
                        warn("AdHocStore.prune cannot date {}; keeping it", child.string());
                    }
                }
                continue;
            }

            // A loose file (the `dialog_<name>.wav` the old sweep could never
            // see) or a per-run directory.
            ++report.entriesScanned;
            if (auto candidate = candidateFor(path)) {
                report.bytesRetained += candidate->bytes;
                candidates.push_back(*candidate);
            } else {
                ++report.skippedEntries;
                warn("AdHocStore.prune cannot date {}; keeping it", path.string());
            }
        }
    }

    creatures::util::applyPrunePolicyTiers(policy, candidates, report, evict, "Ad-hoc store");

    if (span) {
        span->setAttribute("cache.prune.entries_scanned", static_cast<int64_t>(report.entriesScanned));
        span->setAttribute("cache.prune.temporaries", static_cast<int64_t>(report.temporaryFiles));
        span->setAttribute("cache.prune.idle", static_cast<int64_t>(report.idleEntries));
        span->setAttribute("cache.prune.oversize", static_cast<int64_t>(report.oversizeEntries));
        span->setAttribute("cache.prune.protected", static_cast<int64_t>(report.protectedEntries));
        span->setAttribute("cache.prune.skipped", static_cast<int64_t>(report.skippedEntries));
        span->setAttribute("cache.prune.bytes_reclaimed", static_cast<int64_t>(report.bytesReclaimed));
        span->setAttribute("cache.prune.bytes_retained", static_cast<int64_t>(report.bytesRetained));
        span->setAttribute("cache.prune.aborted", report.aborted);
        span->setSuccess();
    }

    info("Ad-hoc prune ({}{}): scanned {} entr(ies), {} temporaries, {} idle, {} over cap, {} too new, {} skipped, "
         "{} MB reclaimed, {} MB retained",
         report.dryRun ? "preview" : "applied", report.aborted ? ", aborted" : "", report.entriesScanned,
         report.temporaryFiles, report.idleEntries, report.oversizeEntries, report.protectedEntries,
         report.skippedEntries, report.bytesReclaimed / (1024 * 1024), report.bytesRetained / (1024 * 1024));

    return Result<CachePruneReport>{report};
}

} // namespace creatures::storage
