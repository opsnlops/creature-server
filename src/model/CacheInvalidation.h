
#pragma once

#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/namespace-stuffs.h"

namespace creatures {

// NOTE: When adding new types, be sure and update CacheInvalidation.cpp, too.
enum class CacheType {
    Animation,
    Creature,
    Playlist,
    SoundList,
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

#include OATPP_CODEGEN_BEGIN(DTO)
class CacheInvalidationDto : public oatpp::DTO {

    DTO_INIT(CacheInvalidationDto, DTO /* extends */)

    DTO_FIELD_INFO(cache_type) {
        info->description = "A string representation of the type of cache that "
                            "should be invalidated";
    }
    DTO_FIELD(String, cache_type);
};
#include OATPP_CODEGEN_END(DTO)

oatpp::Object<CacheInvalidationDto> convertToDto(const CacheInvalidation &cacheInvalidation);
creatures::CacheInvalidation convertFromDto(const std::shared_ptr<CacheInvalidationDto> &cacheInvalidationDto);

} // namespace creatures