#include <memory>
#include <optional>

#include <fmt/format.h>

#include "server/creature/UniverseResolver.h"
#include "server/namespace-stuffs.h"
#include "util/cache.h"
#include "util/helpers.h"

namespace creatures {

extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;

Result<PlaybackUniverse> resolvePlaybackUniverse(const std::vector<creatureId_t> &creatureIds) {

    if (!creatureUniverseMap) {
        return Result<PlaybackUniverse>{
            ServerError(ServerError::InternalError, "Creature/universe map is unavailable")};
    }

    if (creatureIds.empty()) {
        return Result<PlaybackUniverse>{
            ServerError(ServerError::InvalidData, "No creatures given to resolve a universe for")};
    }

    PlaybackUniverse resolved;
    std::optional<universe_t> common;

    for (const auto &creatureId : creatureIds) {
        const auto universePtr = creatureUniverseMap->tryGet(creatureId);
        if (!universePtr) {
            // Powered off, or never seen since the server booted. The scene
            // keeps it: its audio belongs in the room regardless (issue #233).
            resolved.unregistered.push_back(creatureId);
            continue;
        }
        if (!common) {
            common = *universePtr;
        } else if (*common != *universePtr) {
            return Result<PlaybackUniverse>{
                ServerError(ServerError::InvalidData,
                            fmt::format("Creatures span more than one universe ({} and {}); playback targets a "
                                        "single universe",
                                        static_cast<long long>(*common), static_cast<long long>(*universePtr)))};
        }
    }

    if (!common) {
        // Nobody is registered, so there is genuinely nothing to work from —
        // we would be inventing a universe rather than degrading to a known
        // one. Name them all: reporting only the first sent the operator
        // chasing one creature when several were down (issue #233).
        return Result<PlaybackUniverse>{ServerError(
            ServerError::Conflict,
            fmt::format("No creature in this animation is registered with a universe, so there is nowhere to play "
                        "it. Are their controllers online? ({})",
                        joinStrings(resolved.unregistered, ", ")))};
    }

    resolved.universe = *common;
    if (!resolved.unregistered.empty()) {
        warn("Playing into universe {} with {} creature(s) absent: {}. Their audio still plays; only their motion "
             "is missing (issue #233)",
             resolved.universe, resolved.unregistered.size(), joinStrings(resolved.unregistered, ", "));
    }
    return Result<PlaybackUniverse>{std::move(resolved)};
}

} // namespace creatures
