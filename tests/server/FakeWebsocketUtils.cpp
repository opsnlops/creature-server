// Test-only stub for websocketUtils.cpp's free functions. The real
// implementation pulls in the full websocket dependency tree (event loop,
// PlaylistStatus convertToDto, JobState serialization, etc.) which is
// overkill for unit tests that just need the symbols to link.
//
// scheduleCacheInvalidationEvent records into a thread-local log so storage-
// publisher tests can assert that the right CacheTypes fire (and that NO
// invalidation fires on a failed publish). Production code ignores the log.

#include <vector>

#include "model/CacheInvalidation.h"
#include "model/PlaylistStatus.h"
#include "server/jobs/JobState.h"
#include "util/Result.h"
#include "util/websocketUtils.h"

namespace creatures {

namespace {
thread_local std::vector<CacheType> g_invalidationLog;
} // namespace

namespace testing {

const std::vector<CacheType> &scheduledInvalidationsForTesting() { return g_invalidationLog; }
void clearScheduledInvalidationsForTesting() { g_invalidationLog.clear(); }

} // namespace testing

void scheduleCacheInvalidationEvent(framenum_t /*frameOffset*/, CacheType type) { g_invalidationLog.push_back(type); }

Result<bool> broadcastNoticeToAllClients(const std::string & /*message*/) { return Result<bool>{true}; }

Result<bool> broadcastCacheInvalidationToAllClients(const CacheType & /*type*/) { return Result<bool>{true}; }

Result<bool> broadcastPlaylistStatusToAllClients(const PlaylistStatus & /*playlistStatus*/) {
    return Result<bool>{true};
}

Result<bool> broadcastJobProgressToAllClients(const jobs::JobState & /*jobState*/) { return Result<bool>{true}; }

Result<bool> broadcastJobCompleteToAllClients(const jobs::JobState & /*jobState*/) { return Result<bool>{true}; }

} // namespace creatures
