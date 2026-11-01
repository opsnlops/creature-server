#pragma once

#include <memory>
#include <string>

#include "server/transport/HttpTypes.h"
#include "util/websocketUtils.h"

namespace creatures {
class OperationSpan;
}

namespace creatures::transport {

PreparedResponse getJob(const std::string &jobId, const std::shared_ptr<OperationSpan> &span);
PreparedResponse invalidateCache(CacheType type, const std::shared_ptr<OperationSpan> &span);
PreparedResponse pruneAudioCache(const std::string &dryRun, const std::shared_ptr<OperationSpan> &span);
PreparedResponse sendDebugPlaylistUpdate(const std::shared_ptr<OperationSpan> &span);

} // namespace creatures::transport
