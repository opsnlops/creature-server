
#include <string>

#include "model/CacheInvalidation.h"
#include "model/JsonCodec.h"

namespace creatures {

const std::string ANIMATION_CACHE_TYPE = "animation";
const std::string CREATURE_CACHE_TYPE = "creature";
const std::string PLAYLIST_CACHE_TYPE = "playlist";
const std::string SOUND_LIST_CACHE_TYPE = "sound-list";
const std::string ADHOC_ANIMATION_CACHE_TYPE = "ad-hoc-animation-list";
const std::string ADHOC_SOUND_CACHE_TYPE = "ad-hoc-sound-list";
const std::string ADHOC_EXCHANGE_CACHE_TYPE = "ad-hoc-exchange-list";
const std::string FIXTURE_CACHE_TYPE = "fixture";
const std::string DIALOG_SCRIPT_LIST_CACHE_TYPE = "dialog-script-list";
const std::string STORYBOARD_LIST_CACHE_TYPE = "storyboard-list";
const std::string STAGE_LIST_CACHE_TYPE = "stage-list";
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
    case CacheType::AdHocAnimationList:
        return ADHOC_ANIMATION_CACHE_TYPE;
    case CacheType::AdHocSoundList:
        return ADHOC_SOUND_CACHE_TYPE;
    case CacheType::AdHocExchangeList:
        return ADHOC_EXCHANGE_CACHE_TYPE;
    case CacheType::Fixture:
        return FIXTURE_CACHE_TYPE;
    case CacheType::DialogScriptList:
        return DIALOG_SCRIPT_LIST_CACHE_TYPE;
    case CacheType::StoryboardList:
        return STORYBOARD_LIST_CACHE_TYPE;
    case CacheType::StageList:
        return STAGE_LIST_CACHE_TYPE;

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
    if (cacheTypeString == ADHOC_ANIMATION_CACHE_TYPE)
        return CacheType::AdHocAnimationList;
    if (cacheTypeString == ADHOC_SOUND_CACHE_TYPE)
        return CacheType::AdHocSoundList;
    if (cacheTypeString == ADHOC_EXCHANGE_CACHE_TYPE)
        return CacheType::AdHocExchangeList;
    if (cacheTypeString == FIXTURE_CACHE_TYPE)
        return CacheType::Fixture;
    if (cacheTypeString == DIALOG_SCRIPT_LIST_CACHE_TYPE)
        return CacheType::DialogScriptList;
    if (cacheTypeString == STORYBOARD_LIST_CACHE_TYPE)
        return CacheType::StoryboardList;
    if (cacheTypeString == STAGE_LIST_CACHE_TYPE)
        return CacheType::StageList;
    return CacheType::Unknown;
}

nlohmann::json cacheInvalidationToJson(const CacheInvalidation &cacheInvalidation) {
    return {{"cache_type", toString(cacheInvalidation.cache_type)}};
}

Result<CacheInvalidation> cacheInvalidationFromJson(const nlohmann::json &json, std::string_view path) {
    auto fields = json_codec::rejectUnknownFields(json, path, {"cache_type"});
    if (!fields.isSuccess())
        return Result<CacheInvalidation>{fields.getError().value()};
    auto cacheType = json_codec::requiredString(json, path, "cache_type", 64);
    if (!cacheType.isSuccess())
        return Result<CacheInvalidation>{cacheType.getError().value()};

    return Result<CacheInvalidation>{CacheInvalidation{cacheTypeFromString(cacheType.getValue().value())}};
}

} // namespace creatures
