
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/ws/dto/NoticeDto.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

class NoticeMessage : public WebSocketMessageDto<oatpp::Object<creatures::NoticeDto>> {

    DTO_INIT(NoticeMessage, WebSocketMessageDto<oatpp::Object<creatures::NoticeDto>>)
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
