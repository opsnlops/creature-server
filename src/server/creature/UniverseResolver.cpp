
#include <memory>
#include <optional>

#include <fmt/format.h>

#include "server/creature/UniverseResolver.h"
#include "util/cache.h"

namespace creatures {

extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;

Result<universe_t> resolveCommonUniverse(const std::vector<creatureId_t> &creatureIds) {

    if (!creatureUniverseMap) {
        return Result<universe_t>{ServerError(ServerError::InternalError, "Creature/universe map is unavailable")};
    }

    if (creatureIds.empty()) {
        return Result<universe_t>{
            ServerError(ServerError::InvalidData, "No creatures given to resolve a universe for")};
    }

    std::optional<universe_t> common;
    for (const auto &creatureId : creatureIds) {
        const auto universePtr = creatureUniverseMap->tryGet(creatureId);
        if (!universePtr) {
            return Result<universe_t>{ServerError(
                ServerError::Conflict,
                fmt::format("Creature {} is not registered with a universe. Is the controller online?", creatureId))};
        }
        if (!common) {
            common = *universePtr;
        } else if (*common != *universePtr) {
            return Result<universe_t>{
                ServerError(ServerError::InvalidData,
                            fmt::format("Creatures span more than one universe ({} and {}); playback targets a "
                                        "single universe",
                                        static_cast<long long>(*common), static_cast<long long>(*universePtr)))};
        }
    }

    return Result<universe_t>{*common};
}

} // namespace creatures
