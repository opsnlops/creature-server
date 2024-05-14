
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/Types.hpp>

#include "model/Notice.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

    class NoticeMessageCommandDTO : public WebSocketMessageDto<oatpp::Object<creatures::NoticeDto>> {

        DTO_INIT(NoticeMessageCommandDTO, WebSocketMessageDto<oatpp::Object<creatures::NoticeDto>>)

    };

}
#include OATPP_CODEGEN_END(DTO)