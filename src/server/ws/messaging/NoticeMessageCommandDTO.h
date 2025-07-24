
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/Notice.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

class NoticeMessageCommandDTO : public WebSocketMessageDto<oatpp::Object<creatures::NoticeDto>> {

    DTO_INIT(NoticeMessageCommandDTO, WebSocketMessageDto<oatpp::Object<creatures::NoticeDto>>)
};

} // namespace creatures::ws
#include OATPP_CODEGEN_END(DTO)