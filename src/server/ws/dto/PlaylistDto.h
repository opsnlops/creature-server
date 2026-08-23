#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/Playlist.h"
#include "server/ws/dto/PlaylistItemDto.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class PlaylistDto : public oatpp::DTO {
    DTO_INIT(PlaylistDto, DTO)

    DTO_FIELD_INFO(id) { info->description = "The ID of this playlist in the form of an UUID"; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(name) { info->description = "The name of this playlist in the UI"; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(items) { info->description = "The items in the playlist"; }
    DTO_FIELD(List<Object<PlaylistItemDto>>, items);

    DTO_FIELD_INFO(number_of_items) { info->description = "The number of items in this playlist"; }
    DTO_FIELD(UInt32, number_of_items);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<PlaylistDto> convertToDto(const Playlist &playlist);
Playlist convertFromDto(const oatpp::Object<PlaylistDto> &playlistDto);

} // namespace creatures
