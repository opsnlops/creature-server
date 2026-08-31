#include <gtest/gtest.h>

#include "api/DebugResponses.h"

namespace creatures::api {
namespace {

TEST(DebugResponses, PreservesAudioCachePruneWireShape) {
    const AudioCachePruneResponse response{true, 12, 3, 2, 1, 4, 4096, {"orphan-a", "orphan-b"}};

    EXPECT_EQ(audioCachePruneResponseToJson(response),
              (nlohmann::json{{"dry_run", true},
                              {"entries_scanned", 12},
                              {"orphaned_entries", 3},
                              {"incomplete_entries", 2},
                              {"temporary_files", 1},
                              {"orphaned_lock_files", 4},
                              {"bytes_reclaimed", 4096},
                              {"removed", {"orphan-a", "orphan-b"}}}));
}

} // namespace
} // namespace creatures::api
