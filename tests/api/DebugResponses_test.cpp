#include <gtest/gtest.h>

#include "api/DebugResponses.h"

namespace creatures::api {
namespace {

TEST(DebugResponses, PreservesAudioCachePruneWireShape) {
    const AudioCachePruneResponse response{.dryRun = true,
                                           .entriesScanned = 12,
                                           .orphanedEntries = 3,
                                           .incompleteEntries = 2,
                                           .temporaryFiles = 1,
                                           .orphanedLockFiles = 4,
                                           .idleEntries = 5,
                                           .oversizeEntries = 6,
                                           .skippedEntries = 7,
                                           .bytesReclaimed = 4096,
                                           .bytesRetained = 8192,
                                           .aborted = false,
                                           .removed = {"orphan-a", "orphan-b"}};

    EXPECT_EQ(audioCachePruneResponseToJson(response), (nlohmann::json{{"dry_run", true},
                                                                       {"entries_scanned", 12},
                                                                       {"orphaned_entries", 3},
                                                                       {"incomplete_entries", 2},
                                                                       {"temporary_files", 1},
                                                                       {"orphaned_lock_files", 4},
                                                                       {"idle_entries", 5},
                                                                       {"oversize_entries", 6},
                                                                       {"skipped_entries", 7},
                                                                       {"bytes_reclaimed", 4096},
                                                                       {"bytes_retained", 8192},
                                                                       {"aborted", false},
                                                                       {"removed", {"orphan-a", "orphan-b"}}}));
}

} // namespace
} // namespace creatures::api
