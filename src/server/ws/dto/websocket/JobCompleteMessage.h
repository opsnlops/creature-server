
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/ws/dto/JobCompleteDto.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * WebSocket message for job completion notifications
 */
class JobCompleteMessage : public WebSocketMessageDto<oatpp::Object<JobCompleteDto>> {
    DTO_INIT(JobCompleteMessage, WebSocketMessageDto<oatpp::Object<JobCompleteDto>>)
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
