#pragma once

#include <memory>
#include <string>

#include "server/transport/HttpTypes.h"

namespace creatures {
class OperationSpan;
}
namespace creatures::transport {
PreparedResponse listDialogScripts(const std::shared_ptr<OperationSpan> &span);
PreparedResponse getDialogScript(const std::string &scriptId, const std::shared_ptr<OperationSpan> &span);
PreparedResponse createDialogScript(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse updateDialogScript(const std::string &scriptId, const std::string &body,
                                    const std::shared_ptr<OperationSpan> &span);
PreparedResponse validateDialogScript(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse clearDialogMusic(const std::string &scriptId, const std::shared_ptr<OperationSpan> &span);
PreparedResponse clearDialogVoice(const std::string &scriptId, const std::shared_ptr<OperationSpan> &span);
PreparedResponse deleteDialogScript(const std::string &scriptId, const std::shared_ptr<OperationSpan> &span);
} // namespace creatures::transport
