#pragma once

#include <memory>
#include <string>

#include "server/transport/HttpTypes.h"

namespace creatures {
class OperationSpan;
}

namespace creatures::transport {

PreparedResponse startDialogStream(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse addDialogStreamTurn(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse finishDialogStream(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse startStreamingAdHoc(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse addStreamingAdHocText(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse finishStreamingAdHoc(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse listStreamingAdHocExchanges(const std::string &limit, const std::shared_ptr<OperationSpan> &span);
PreparedResponse getStreamingAdHocExchange(const std::string &sessionId, const std::shared_ptr<OperationSpan> &span);

} // namespace creatures::transport
