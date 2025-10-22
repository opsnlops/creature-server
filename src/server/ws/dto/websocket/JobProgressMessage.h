
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/ws/dto/JobProgressDto.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * WebSocket message for job progress updates
 */
class JobProgressMessage : public WebSocketMessageDto<oatpp::Object<JobProgressDto>> {
    DTO_INIT(JobProgressMessage, WebSocketMessageDto<oatpp::Object<JobProgressDto>>)
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
