
#pragma once

#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>



#include "model/AnimationMetadata.h"
#include "model/FrameData.h"

namespace creatures {

    struct Animation {
        std::string id;
        AnimationMetadata metadata;
        std::vector<FrameData> tracks;
    };

#include OATPP_CODEGEN_BEGIN(DTO)

    class AnimationDto : public oatpp::DTO {

        DTO_INIT(AnimationDto, DTO /* extends */);

        DTO_FIELD_INFO(id) {
            info->description = "Animation ID in the form of a MongoDB OID";
        }
        DTO_FIELD(String, id);

        DTO_FIELD_INFO(metadata) {
            info->description = "An array of AnimationMetadataDto objects that describe the animations";
        }
        DTO_FIELD(Object<AnimationMetadataDto>, metadata);

        DTO_FIELD_INFO(tracks) {
            info->description = "Frame data for the animation tracks";
        }
        DTO_FIELD(Vector<oatpp::Object<FrameDataDto>>, tracks);

    };

#include OATPP_CODEGEN_END(DTO)

    std::shared_ptr<AnimationDto> convertToDto(const Animation &creature);
    Animation convertFromDto(const std::shared_ptr<AnimationDto> &creatureDto);


}

