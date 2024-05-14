
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/Types.hpp>

#include "model/StreamFrame.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

    class StreamFrameCommandDTO : public WebSocketMessageDto<oatpp::Object<creatures::StreamFrameDto>> {

        DTO_INIT(StreamFrameCommandDTO, WebSocketMessageDto<oatpp::Object<creatures::StreamFrameDto>>)

    };

}
#include OATPP_CODEGEN_END(DTO)