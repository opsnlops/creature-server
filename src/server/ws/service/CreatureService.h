
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/core/macro/component.hpp>

#include "model/Creature.h"
#include "server/ws/dto/PageDto.h"

namespace creatures :: ws {

    class CreatureService {

    private:
        typedef oatpp::web::protocol::http::Status Status;

    public:

        oatpp::Object<PageDto<oatpp::Object<creatures::CreatureDto>>> getAllCreatures();

        oatpp::Object<creatures::CreatureDto> getCreature(const oatpp::String& inCreatureId);

    };


} // creatures :: ws
