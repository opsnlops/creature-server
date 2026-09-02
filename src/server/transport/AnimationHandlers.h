#pragma once

#include <memory>
#include <string>

#include "server/transport/HttpTypes.h"

namespace creatures {
class OperationSpan;
}

namespace creatures::transport {
PreparedResponse listAnimations(const std::shared_ptr<OperationSpan> &span);
PreparedResponse listAdHocAnimations(const std::shared_ptr<OperationSpan> &span);
PreparedResponse getAnimation(const std::string &id, bool adHoc, const std::shared_ptr<OperationSpan> &span);
PreparedResponse upsertAnimation(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse deleteAnimation(const std::string &id, const std::shared_ptr<OperationSpan> &span);
PreparedResponse playAnimation(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse regenerateAnimationLipSync(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse createAdHocAnimation(const std::string &body, bool autoPlay,
                                      const std::shared_ptr<OperationSpan> &span);
} // namespace creatures::transport
