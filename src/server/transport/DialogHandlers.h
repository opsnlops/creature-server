#pragma once

#include <memory>
#include <string>

#include "server/transport/HttpTypes.h"

namespace creatures {
class OperationSpan;
}

namespace creatures::transport {

/// POST /api/v1/animation/dialog — assemble a scene as a job (202).
PreparedResponse submitDialog(const std::string &body, const std::shared_ptr<OperationSpan> &span);

/// POST /api/v1/animation/dialog/voice/accept — 200 with the script, or 202 when the take must be assembled.
PreparedResponse acceptVoiceTake(const std::string &body, const std::shared_ptr<OperationSpan> &span);

/// POST /api/v1/animation/dialog/preview/lookup — what is cached for these turns.
PreparedResponse lookupDialogPreview(const std::string &body, const std::shared_ptr<OperationSpan> &span);

/// POST /api/v1/animation/dialog/preview/meta — 200 from cache, else a job (202).
PreparedResponse submitDialogPreviewMeta(const std::string &body, const std::shared_ptr<OperationSpan> &span);

/// POST /api/v1/animation/dialog/preview/multichannel — always a job (202).
PreparedResponse submitDialogPreviewMultichannel(const std::string &body, const std::shared_ptr<OperationSpan> &span);

} // namespace creatures::transport
