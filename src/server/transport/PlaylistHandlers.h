#pragma once

#include <memory>
#include <string>

#include "server/transport/HttpTypes.h"

namespace creatures {
class OperationSpan;
}

namespace creatures::transport {

PreparedResponse listPlaylists(const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse getPlaylist(const std::string &playlistId, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse upsertPlaylist(const std::string &body, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse startPlaylist(const std::string &body, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse stopPlaylist(const std::string &body, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse getPlaylistStatus(const std::string &universe, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse listPlaylistStatuses(const std::shared_ptr<OperationSpan> &parentSpan);

} // namespace creatures::transport
