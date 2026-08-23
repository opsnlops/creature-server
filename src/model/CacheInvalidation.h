
#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures {

// NOTE: When adding new types, be sure and update CacheInvalidation.cpp, too.
enum class CacheType {
    Animation,
    Creature,
    Playlist,
    SoundList,
    AdHocAnimationList,
    AdHocSoundList,
    AdHocExchangeList,
    Fixture,
    DialogScriptList,
    StoryboardList,
    StageList,
    Unknown,
};

/**
 * Helper function to map a cache type to a string
 *
 * @param type the CacheType
 * @return a string representation of it
 */
std::string toString(CacheType type);

/**
 * Map a string to a CacheType
 *
 * @param cacheTypeString the string to decode
 * @return a `CacheType` with the status of the decode
 */
CacheType cacheTypeFromString(const std::string &cacheTypeString);

/**
 * A signal to clients that they should invalidate a cache they have (if any)
 * for a particular object type.
 *
 * Clients are not required to cache things, but eh Creature Console certainly
 * does. This is a hint from the server that the state on it's side has changed
 * and it should invalidate its own cache and re-pull it.
 *
 * This is one of the two hard things to do in Computer Science, after all! 😅
 *
 */
struct CacheInvalidation {
    CacheType cache_type;
};

nlohmann::json cacheInvalidationToJson(const CacheInvalidation &cacheInvalidation);
Result<CacheInvalidation> cacheInvalidationFromJson(const nlohmann::json &json,
                                                    std::string_view path = "cache_invalidation");

} // namespace creatures
