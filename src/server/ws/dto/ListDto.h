
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "server/ws/dto/AnimationDto.h"

#include OATPP_CODEGEN_BEGIN(DTO)

template <class T> class ListDto : public oatpp::DTO {

    DTO_INIT(ListDto, DTO)

    DTO_FIELD(UInt32, count);
    DTO_FIELD(Vector<T>, items);
};

class AnimationsListDto : public ListDto<oatpp::Object<creatures::AnimationMetadataDto>> {
    DTO_INIT(AnimationsListDto, ListDto<oatpp::Object<creatures::AnimationMetadataDto>>)
};

#include OATPP_CODEGEN_END(DTO)
