
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/Http.hpp>

#include "model/Creature.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/SimpleResponseDto.h"
#include "server/ws/dto/StatusDto.h"

namespace creatures ::ws {

class CreatureService {

  private:
    typedef oatpp::web::protocol::http::Status Status;

  public:
    static oatpp::Object<ListDto<oatpp::Object<creatures::CreatureDto>>>
    getAllCreatures(std::shared_ptr<RequestSpan> parentSpan = nullptr);

    static oatpp::Object<creatures::CreatureDto> getCreature(const oatpp::String &inCreatureId,
                                                             std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Upsert (create or update) a creature
     *
     * @param jsonCreature a JSON representation of the creature. All fields will be stored in MongoDB, but the
     *                     required fields must me present.
     * @return the creature that was created or updated in the standard form
     */
    static oatpp::Object<creatures::CreatureDto> upsertCreature(const std::string &jsonCreature,
                                                                std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Register a creature with its universe assignment
     *
     * Called by controllers when they start up. This upserts the creature config to the database
     * and stores the creature-to-universe mapping in runtime memory.
     *
     * @param jsonCreature a JSON representation of the creature configuration from the controller
     * @param universe the universe this creature is currently assigned to
     * @return the creature that was registered
     */
    static oatpp::Object<creatures::CreatureDto> registerCreature(const std::string &jsonCreature, universe_t universe,
                                                                  std::shared_ptr<RequestSpan> parentSpan = nullptr);
};

} // namespace creatures::ws
