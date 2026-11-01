#pragma once

#include <memory>
#include <string>

#include "server/transport/HttpTypes.h"

namespace creatures {
class OperationSpan;
}

namespace creatures::transport {

PreparedResponse listStages(const std::shared_ptr<OperationSpan> &span);
PreparedResponse getStage(const std::string &stageId, const std::shared_ptr<OperationSpan> &span);
PreparedResponse createStage(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse updateStage(const std::string &stageId, const std::string &body,
                             const std::shared_ptr<OperationSpan> &span);
PreparedResponse deleteStage(const std::string &stageId, const std::shared_ptr<OperationSpan> &span);
PreparedResponse listStageAnimations(const std::string &stageId, const std::shared_ptr<OperationSpan> &span);
PreparedResponse rerenderStage(const std::string &stageId, const std::string &body,
                               const std::shared_ptr<OperationSpan> &span);
PreparedResponse rerenderAnimation(const std::string &animationId, const std::string &body,
                                   const std::shared_ptr<OperationSpan> &span);

PreparedResponse listStoryboards(const std::shared_ptr<OperationSpan> &span);
PreparedResponse getStoryboard(const std::string &storyboardId, const std::shared_ptr<OperationSpan> &span);
PreparedResponse createStoryboard(const std::string &body, const std::shared_ptr<OperationSpan> &span);
PreparedResponse updateStoryboard(const std::string &storyboardId, const std::string &body,
                                  const std::shared_ptr<OperationSpan> &span);
PreparedResponse deleteStoryboard(const std::string &storyboardId, const std::shared_ptr<OperationSpan> &span);

} // namespace creatures::transport
