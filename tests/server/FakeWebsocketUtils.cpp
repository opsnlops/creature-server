// Test-only stub for websocketUtils.cpp's free functions. The real
// implementation pulls in the full websocket dependency tree (event loop,
// PlaylistStatus convertToDto, JobState serialization, etc.) which is
// overkill for unit tests that just need the symbols to link.

#include <vector>

#include "model/CacheInvalidation.h"
#include "model/PlaylistStatus.h"
#include "server/jobs/JobState.h"
#include "util/Result.h"
#include "util/websocketUtils.h"

namespace creatures {

void scheduleCacheInvalidationEvent(framenum_t /*frameOffset*/, CacheType /*type*/) {
    // no-op
}

Result<bool> broadcastNoticeToAllClients(const std::string & /*message*/) { return Result<bool>{true}; }

Result<bool> broadcastCacheInvalidationToAllClients(const CacheType & /*type*/) { return Result<bool>{true}; }

Result<bool> broadcastPlaylistStatusToAllClients(const PlaylistStatus & /*playlistStatus*/) {
    return Result<bool>{true};
}

Result<bool> broadcastJobProgressToAllClients(const jobs::JobState & /*jobState*/) { return Result<bool>{true}; }

Result<bool> broadcastJobCompleteToAllClients(const jobs::JobState & /*jobState*/) { return Result<bool>{true}; }

} // namespace creatures
