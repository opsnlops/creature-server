
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/core/macro/component.hpp>

#include "model/Creature.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/SimpleResponseDto.h"
#include "server/ws/dto/StatusDto.h"

namespace creatures :: ws {

    class CreatureService {

    private:
        typedef oatpp::web::protocol::http::Status Status;

    public:

        oatpp::Object<ListDto<oatpp::Object<creatures::CreatureDto>>> getAllCreatures();

        oatpp::Object<creatures::CreatureDto> getCreature(const oatpp::String& inCreatureId);

        /**
         * Upsert (create or update) a creature
         *
         * @param jsonCreature a JSON representation of the creature. All fields will be stored in MongoDB, but the
         *                     required fields must me present.
         * @return the creature that was created or updated in the standard form
         */
        oatpp::Object<creatures::CreatureDto> upsertCreature(const std::string& jsonCreature);
    };


} // creatures :: ws
