#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/ws/dto/websocket/WebSocketMessageDto.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

class IdleStateChangedDto : public oatpp::DTO {
    DTO_INIT(IdleStateChangedDto, DTO)

    DTO_FIELD(String, creature_id);
    DTO_FIELD(Boolean, idle_enabled);
    DTO_FIELD(String, timestamp); // ISO8601
};

class CreatureActivityDto : public oatpp::DTO {
    DTO_INIT(CreatureActivityDto, DTO)

    DTO_FIELD(String, creature_id);
    DTO_FIELD(String, state);        // running|idle|disabled|stopped
    DTO_FIELD(String, animation_id); // nullable
    DTO_FIELD(String, session_id);   // nullable UUIDv4
    DTO_FIELD(String, reason);       // play|ad_hoc|playlist|idle|disabled
    DTO_FIELD(String, timestamp);    // ISO8601
};

class IdleStateChangedMessage : public WebSocketMessageDto<oatpp::Object<IdleStateChangedDto>> {
    DTO_INIT(IdleStateChangedMessage, WebSocketMessageDto<oatpp::Object<IdleStateChangedDto>>)
};

class CreatureActivityMessage : public WebSocketMessageDto<oatpp::Object<CreatureActivityDto>> {
    DTO_INIT(CreatureActivityMessage, WebSocketMessageDto<oatpp::Object<CreatureActivityDto>>)
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
