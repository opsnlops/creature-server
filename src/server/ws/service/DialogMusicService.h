#pragma once

#include <memory>
#include <string>

#include "server/ws/dto/DialogMusicDto.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::ws {

class DialogMusicService {
  public:
    Result<oatpp::Object<DialogMusicGenerationResultDto>> generate(const oatpp::Object<DialogMusicRequestDto> &request,
                                                                   std::shared_ptr<OperationSpan> parentSpan = nullptr,
                                                                   const std::string &jobId = "") const;

    Result<oatpp::Object<DialogMusicPromotionResultDto>>
    promote(const std::string &generationId, std::shared_ptr<RequestSpan> parentSpan = nullptr) const;
};

} // namespace creatures::ws
