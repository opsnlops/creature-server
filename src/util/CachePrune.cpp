//
// CachePrune.cpp — the shared idle + size-cap tiers for on-disk caches.
//

#include "util/CachePrune.h"

#include <algorithm>
#include <system_error>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "server/namespace-stuffs.h"

namespace creatures::util {

std::string CachePrunePolicy::describe() const {
    const auto idle =
        maxIdleAge ? fmt::format("{} day(s)", maxIdleAge->count() / (24 * 3600)) : std::string("disabled");
    const auto cap = maxTotalBytes > 0 ? fmt::format("{} MB", maxTotalBytes / (1024 * 1024)) : std::string("unbounded");
    if (minAge) {
        return fmt::format("idle eviction {}, size cap {}, protecting anything newer than {}s", idle, cap,
                           minAge->count());
    }
    return fmt::format("idle eviction {}, size cap {}", idle, cap);
}

std::uintmax_t directorySizeBytes(const std::filesystem::path &dir) {
    std::uintmax_t bytes = 0;
    std::error_code ec;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        std::error_code sizeError;
        if (entry.is_regular_file(sizeError) && !sizeError) {
            bytes += entry.file_size(sizeError);
        }
    }
    return bytes;
}

std::optional<std::filesystem::file_time_type> newestWriteTime(const std::filesystem::path &dir) {
    std::error_code ec;
    auto newest = std::filesystem::last_write_time(dir, ec);
    if (ec) {
        return std::nullopt;
    }

    std::error_code walkEc;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(dir, walkEc)) {
        if (walkEc) {
            break;
        }
        std::error_code entryEc;
        const auto when = entry.last_write_time(entryEc);
        if (!entryEc && when > newest) {
            newest = when;
        }
    }
    return newest;
}

void applyPrunePolicyTiers(const CachePrunePolicy &policy, std::vector<PruneCandidate> &candidates,
                           CachePruneReport &report, const PruneEvictFn &evict, const char *subject) {
    const auto now = std::filesystem::file_time_type::clock::now();

    // Anything newer than minAge is off limits to both tiers. Counted, so an
    // operator can tell "nothing was old enough" apart from "nothing matched".
    const auto tooYoung = [&](const PruneCandidate &candidate) {
        if (!policy.minAge) {
            return false;
        }
        return candidate.lastUsed > now - *policy.minAge;
    };

    // --- Tier 2: idle eviction (issue #225) -----------------------------------
    // Usable, but nothing has used it in longer than the policy allows.
    // Deleting it costs regenerating it, which is exactly what happens the
    // first time anything is generated, so the only real risk is doing this to
    // something still in rotation — hence conservative defaults. See config.h.
    if (policy.maxIdleAge && !report.aborted) {
        const auto cutoff = now - *policy.maxIdleAge;
        std::vector<PruneCandidate> retained;
        retained.reserve(candidates.size());
        for (const auto &candidate : candidates) {
            if (candidate.lastUsed < cutoff) {
                if (tooYoung(candidate)) {
                    // Only reachable with minAge > maxIdleAge, which is a
                    // misconfiguration rather than a state — say so once per
                    // entry rather than silently keeping it.
                    ++report.protectedEntries;
                    retained.push_back(candidate);
                    continue;
                }
                if (evict(candidate, "idle")) {
                    ++report.idleEntries;
                    continue;
                }
            }
            retained.push_back(candidate);
        }
        candidates.swap(retained);
    }

    // --- Tier 3: size cap, least-recently-used first (issue #225) -------------
    // The tier that actually BOUNDS the cache. Idle eviction clears the long
    // tail but cannot bound a cache whose entries are all in current use.
    // Engaging this tier means the cache outgrew its budget, so it is loud.
    if (policy.maxTotalBytes > 0 && !report.aborted && report.bytesRetained > policy.maxTotalBytes) {
        warn("{} holds {} bytes, over its {} byte ceiling; evicting least-recently-used entries", subject,
             report.bytesRetained, policy.maxTotalBytes);
        std::sort(candidates.begin(), candidates.end(), [](const PruneCandidate &left, const PruneCandidate &right) {
            return left.lastUsed < right.lastUsed;
        });
        for (const auto &candidate : candidates) {
            if (report.bytesRetained <= policy.maxTotalBytes) {
                break;
            }
            if (tooYoung(candidate)) {
                ++report.protectedEntries;
                continue;
            }
            if (evict(candidate, "over size cap")) {
                ++report.oversizeEntries;
            }
        }
        if (report.bytesRetained > policy.maxTotalBytes) {
            warn("{} is still {} bytes after pruning: every remaining entry is live, locked or too new", subject,
                 report.bytesRetained);
        }
    }
}

} // namespace creatures::util
