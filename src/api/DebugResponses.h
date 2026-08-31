#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace creatures::api {

struct AudioCachePruneResponse {
    bool dryRun;
    std::size_t entriesScanned;
    std::size_t orphanedEntries;
    std::size_t incompleteEntries;
    std::size_t temporaryFiles;
    std::size_t orphanedLockFiles;
    std::uintmax_t bytesReclaimed;
    std::vector<std::string> removed;
};

inline nlohmann::json audioCachePruneResponseToJson(const AudioCachePruneResponse &response) {
    return {{"dry_run", response.dryRun},
            {"entries_scanned", response.entriesScanned},
            {"orphaned_entries", response.orphanedEntries},
            {"incomplete_entries", response.incompleteEntries},
            {"temporary_files", response.temporaryFiles},
            {"orphaned_lock_files", response.orphanedLockFiles},
            {"bytes_reclaimed", response.bytesReclaimed},
            {"removed", response.removed}};
}

} // namespace creatures::api
