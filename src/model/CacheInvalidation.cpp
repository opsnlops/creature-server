
#include <string>

#include <oatpp/core/Types.hpp>

#include "model/CacheInvalidation.h"

namespace creatures {

const std::string ANIMATION_CACHE_TYPE = "animation";
const std::string CREATURE_CACHE_TYPE = "creature";
const std::string PLAYLIST_CACHE_TYPE = "playlist";
const std::string SOUND_LIST_CACHE_TYPE = "sound-list";
const std::string UNKNOWN_CACHE_TYPE = "unknown";

std::string toString(const CacheType type) {
    switch (type) {

    case CacheType::Animation:
        return ANIMATION_CACHE_TYPE;
    case CacheType::Creature:
        return CREATURE_CACHE_TYPE;
    case CacheType::Playlist:
        return PLAYLIST_CACHE_TYPE;
    case CacheType::SoundList:
        return SOUND_LIST_CACHE_TYPE;

    default:
        return UNKNOWN_CACHE_TYPE;
    }
}

CacheType cacheTypeFromString(const std::string &cacheTypeString) {
    if (cacheTypeString == ANIMATION_CACHE_TYPE)
        return CacheType::Animation;
    if (cacheTypeString == CREATURE_CACHE_TYPE)
        return CacheType::Creature;
    if (cacheTypeString == PLAYLIST_CACHE_TYPE)
        return CacheType::Playlist;
    if (cacheTypeString == SOUND_LIST_CACHE_TYPE)
        return CacheType::SoundList;
    return CacheType::Unknown;
}

oatpp::Object<CacheInvalidationDto> convertToDto(const CacheInvalidation &cacheInvalidation) {
    auto cacheInvalidationDto = CacheInvalidationDto::createShared();
    cacheInvalidationDto->cache_type = toString(cacheInvalidation.cache_type);

    return cacheInvalidationDto;
}

CacheInvalidation convertFromDto(const std::shared_ptr<CacheInvalidationDto> &cacheInvalidationDto) {
    CacheInvalidation cacheInvalidation{};
    cacheInvalidation.cache_type = cacheTypeFromString(cacheInvalidationDto->cache_type);

    return cacheInvalidation;
}

} // namespace creatures
