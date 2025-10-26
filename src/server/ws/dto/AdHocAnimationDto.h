#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/AnimationMetadata.h"

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

class AdHocAnimationDto : public oatpp::DTO {

    DTO_INIT(AdHocAnimationDto, DTO)

    DTO_FIELD_INFO(animation_id) { info->description = "Unique identifier for the ad-hoc animation"; }
    DTO_FIELD(String, animation_id);

    DTO_FIELD_INFO(metadata) { info->description = "Metadata for the ad-hoc animation"; }
    DTO_FIELD(Object<AnimationMetadataDto>, metadata);

    DTO_FIELD_INFO(created_at) { info->description = "Creation timestamp in ISO8601 format"; }
    DTO_FIELD(String, created_at);
};

class AdHocAnimationListDto : public oatpp::DTO {

    DTO_INIT(AdHocAnimationListDto, DTO)

    DTO_FIELD_INFO(count) { info->description = "Number of ad-hoc animations returned"; }
    DTO_FIELD(UInt32, count);

    DTO_FIELD_INFO(items) { info->description = "Ad-hoc animations"; }
    DTO_FIELD(List<Object<AdHocAnimationDto>>, items);
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
