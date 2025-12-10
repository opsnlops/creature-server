
#pragma once

#include "spdlog/spdlog.h"

#include <utility>
#include <vector>

#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/Http.hpp>

#include "model/Creature.h"
#include "server/runtime/Activity.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/SimpleResponseDto.h"
#include "server/ws/dto/StatusDto.h"

namespace creatures {
class RequestSpan;
class OperationSpan;
} // namespace creatures

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

    /**
     * Toggle idle enabled/disabled for a creature (runtime only)
     */
    static oatpp::Object<creatures::CreatureDto> setIdleEnabled(const oatpp::String &inCreatureId, bool enabled,
                                                                std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Update runtime activity state for creatures (runtime only)
     *
     * @param creatureIds list of creature IDs involved
     * @param animationId animation identifier (cleared automatically when state is not running)
     * @param reason descriptive reason (play|ad_hoc|playlist|idle|disabled|cancelled)
     * @param state activity state (running|idle|disabled|stopped)
     * @param sessionId optional session UUID (if empty, a new UUIDv4 is generated)
     */
    static std::string setActivityState(const std::vector<creatureId_t> &creatureIds, const std::string &animationId,
                                        runtime::ActivityReason reason, runtime::ActivityState state,
                                        const std::string &sessionId = "",
                                        std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Convenience wrapper for marking creatures as running a specific animation
     */
    static std::string setActivityRunning(const std::vector<creatureId_t> &creatureIds, const std::string &animationId,
                                          runtime::ActivityReason reason, const std::string &sessionId = "",
                                          std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Check if a creature is currently in streaming mode (runtime-only).
     */
    static bool isCreatureStreaming(const creatureId_t &creatureId);

    /**
     * Snapshot of all creature runtime states currently held in memory.
     */
    static std::vector<std::pair<std::string, oatpp::Object<creatures::CreatureRuntimeDto>>> getRuntimeStates();
};

} // namespace creatures::ws
