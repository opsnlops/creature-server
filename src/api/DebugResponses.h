#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace creatures::api {

struct AudioCachePruneResponse {
    bool dryRun{true};
    std::size_t entriesScanned{0};
    std::size_t orphanedEntries{0};
    std::size_t incompleteEntries{0};
    std::size_t temporaryFiles{0};
    std::size_t orphanedLockFiles{0};
    // Usable entries the policy chose to drop anyway (issue #225): unread past
    // the idle age, or evicted least-recently-read to fit the size cap.
    std::size_t idleEntries{0};
    std::size_t oversizeEntries{0};
    // Candidates deliberately left alone — locked, read since they were
    // sampled, or a source that could not be statted.
    std::size_t skippedEntries{0};
    std::uintmax_t bytesReclaimed{0};
    std::uintmax_t bytesRetained{0};
    bool aborted{false};
    std::vector<std::string> removed;
};

inline nlohmann::json audioCachePruneResponseToJson(const AudioCachePruneResponse &response) {
    return {{"dry_run", response.dryRun},
            {"entries_scanned", response.entriesScanned},
            {"orphaned_entries", response.orphanedEntries},
            {"incomplete_entries", response.incompleteEntries},
            {"temporary_files", response.temporaryFiles},
            {"orphaned_lock_files", response.orphanedLockFiles},
            {"idle_entries", response.idleEntries},
            {"oversize_entries", response.oversizeEntries},
            {"skipped_entries", response.skippedEntries},
            {"bytes_reclaimed", response.bytesReclaimed},
            {"bytes_retained", response.bytesRetained},
            {"aborted", response.aborted},
            {"removed", response.removed}};
}

} // namespace creatures::api
