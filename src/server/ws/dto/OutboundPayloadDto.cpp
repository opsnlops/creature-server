#include "server/ws/dto/CacheInvalidationDto.h"
#include "server/ws/dto/NoticeDto.h"
#include "server/ws/dto/PlaylistStatusDto.h"

namespace creatures {

oatpp::Object<PlaylistStatusDto> convertToDto(const PlaylistStatus &playlistStatus) {
    auto dto = PlaylistStatusDto::createShared();
    dto->universe = playlistStatus.universe;
    dto->playlist = playlistStatus.playlist;
    dto->playing = playlistStatus.playing;
    dto->current_animation = playlistStatus.current_animation;
    return dto;
}

PlaylistStatus convertFromDto(const oatpp::Object<PlaylistStatusDto> &dto) {
    return PlaylistStatus{dto->universe, dto->playlist, dto->playing, dto->current_animation};
}

oatpp::Object<NoticeDto> convertToDto(const Notice &notice) {
    auto dto = NoticeDto::createShared();
    dto->timestamp = notice.timestamp;
    dto->message = notice.message;
    return dto;
}

Notice convertFromDto(const oatpp::Object<NoticeDto> &dto) { return Notice{dto->timestamp, dto->message}; }

oatpp::Object<CacheInvalidationDto> convertToDto(const CacheInvalidation &cacheInvalidation) {
    auto dto = CacheInvalidationDto::createShared();
    dto->cache_type = toString(cacheInvalidation.cache_type);
    return dto;
}

CacheInvalidation convertFromDto(const oatpp::Object<CacheInvalidationDto> &dto) {
    return CacheInvalidation{cacheTypeFromString(dto->cache_type)};
}

} // namespace creatures
