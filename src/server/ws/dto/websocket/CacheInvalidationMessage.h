
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/Types.hpp>

#include "model/CacheInvalidation.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"


namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)
    class CacheInvalidationMessage : public WebSocketMessageDto<oatpp::Object<creatures::CacheInvalidationDto>> {
        DTO_INIT(CacheInvalidationMessage, WebSocketMessageDto<oatpp::Object<creatures::CacheInvalidationDto>>)
    };
#include OATPP_CODEGEN_END(DTO)

}