
#include <string>

#include <oatpp/core/Types.hpp>


#include "model/CacheInvalidation.h"

namespace creatures {

    const std::string ANIMATION_CACHE_TYPE = "animation";
    const std::string CREATURE_CACHE_TYPE = "creature";
    const std::string PLAYLIST_CACHE_TYPE = "playlist";
    const std::string UNKNOWN_CACHE_TYPE = "unknown";

    std::string toString(CacheType type) {
        switch (type) {

            case CacheType::Animation: return ANIMATION_CACHE_TYPE;
            case CacheType::Creature: return CREATURE_CACHE_TYPE;
            case CacheType::Playlist: return PLAYLIST_CACHE_TYPE;

            default: return UNKNOWN_CACHE_TYPE;
        }
    }

    CacheType cacheTypeFromString(const std::string& cacheTypeString) {
        if (cacheTypeString == ANIMATION_CACHE_TYPE) return CacheType::Animation;
        if (cacheTypeString == CREATURE_CACHE_TYPE) return CacheType::Creature;
        if (cacheTypeString == PLAYLIST_CACHE_TYPE) return CacheType::Playlist;
        return CacheType::Unknown;
    }

     oatpp::Object<CacheInvalidationDto> convertToDto(const CacheInvalidation &cacheInvalidation) {
        auto cacheInvalidationDto = CacheInvalidationDto::createShared();
        cacheInvalidationDto->cacheType = toString(cacheInvalidation.cacheType);

        return cacheInvalidationDto;
    }

    CacheInvalidation convertFromDto(const std::shared_ptr<CacheInvalidationDto> &cacheInvalidationDto) {
        CacheInvalidation cacheInvalidation{};
        cacheInvalidation.cacheType = cacheTypeFromString(cacheInvalidationDto->cacheType);

        return cacheInvalidation;
    }

} // namespace creatures
