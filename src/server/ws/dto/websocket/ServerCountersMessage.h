
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/Creature.h"
#include "server/metrics/counters.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

class ServerCountersCreatureRuntimeDto : public oatpp::DTO {

    DTO_INIT(ServerCountersCreatureRuntimeDto, DTO)

    DTO_FIELD_INFO(creature_id) { info->description = "Creature ID for this runtime state entry"; }
    DTO_FIELD(String, creature_id);

    DTO_FIELD_INFO(runtime) { info->description = "Current runtime state for the creature"; }
    DTO_FIELD(Object<creatures::CreatureRuntimeDto>, runtime);
};

class ServerCountersPayloadDto : public oatpp::DTO {

    DTO_INIT(ServerCountersPayloadDto, DTO)

    DTO_FIELD_INFO(counters) { info->description = "Global server counters"; }
    DTO_FIELD(Object<creatures::SystemCountersDto>, counters);

    DTO_FIELD_INFO(runtime_states) { info->description = "Runtime state for all creatures currently in memory"; }
    DTO_FIELD(List<Object<ServerCountersCreatureRuntimeDto>>, runtime_states);
};

class ServerCountersMessage : public WebSocketMessageDto<oatpp::Object<ServerCountersPayloadDto>> {

    DTO_INIT(ServerCountersMessage, WebSocketMessageDto<oatpp::Object<ServerCountersPayloadDto>>)
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
