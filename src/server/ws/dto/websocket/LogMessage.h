
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/LogItem.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

class LogMessage : public WebSocketMessageDto<oatpp::Object<creatures::LogItemDto>> {

    DTO_INIT(LogMessage, WebSocketMessageDto<oatpp::Object<creatures::LogItemDto>>)
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws