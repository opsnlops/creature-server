
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/Types.hpp>

#include "model/VirtualStatusLights.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

    class VirtualStatusLightsMessage : public WebSocketMessageDto<oatpp::Object<creatures::VirtualStatusLightsDto>> {

        DTO_INIT(VirtualStatusLightsMessage, WebSocketMessageDto<oatpp::Object<creatures::VirtualStatusLightsDto>>)

    };

#include OATPP_CODEGEN_END(DTO)

}