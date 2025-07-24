
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/metrics/counters.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

class ServerCountersMessage : public WebSocketMessageDto<oatpp::Object<creatures::SystemCountersDto>> {

    DTO_INIT(ServerCountersMessage, WebSocketMessageDto<oatpp::Object<creatures::SystemCountersDto>>)
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws