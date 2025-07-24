
#pragma once

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/AnimationMetadata.h"
#include "model/Track.h"

namespace creatures {

struct Animation {
    std::string id;
    AnimationMetadata metadata;
    std::vector<Track> tracks;
};

#include OATPP_CODEGEN_BEGIN(DTO)

class AnimationDto : public oatpp::DTO {

    DTO_INIT(AnimationDto, DTO /* extends */)

    DTO_FIELD_INFO(id) { info->description = "Animation ID in the form of a UUID"; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(metadata) { info->description = "An AnimationMetadataDto with the data for this animation"; }
    DTO_FIELD(Object<AnimationMetadataDto>, metadata);

    DTO_FIELD_INFO(tracks) { info->description = "The tracks of motion data"; }
    DTO_FIELD(Vector<oatpp::Object<TrackDto>>, tracks);
};

#include OATPP_CODEGEN_END(DTO)

std::shared_ptr<AnimationDto> convertToDto(const Animation &creature);
Animation convertFromDto(const std::shared_ptr<AnimationDto> &creatureDto);

} // namespace creatures
