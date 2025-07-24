
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/PlaylistStatus.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)
class PlaylistStatusMessage : public WebSocketMessageDto<oatpp::Object<creatures::PlaylistStatusDto>> {
    DTO_INIT(PlaylistStatusMessage, WebSocketMessageDto<oatpp::Object<creatures::PlaylistStatusDto>>)
};
#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws