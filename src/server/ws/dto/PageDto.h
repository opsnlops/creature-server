
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/Types.hpp>

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "model/Creature.h"


#include OATPP_CODEGEN_BEGIN(DTO)

template<class T>
class PageDto : public oatpp::DTO {

    DTO_INIT(PageDto, DTO)

    DTO_FIELD(UInt32, count);
    DTO_FIELD(Vector<T>, items);

};

class CreaturesPageDto : public PageDto<oatpp::Object<creatures::CreatureDto>> {

DTO_INIT(CreaturesPageDto, PageDto<oatpp::Object<creatures::CreatureDto>>)

};

class AnimationsPageDto : public PageDto<oatpp::Object<creatures::AnimationMetadataDto>> {

    DTO_INIT(AnimationsPageDto, PageDto<oatpp::Object<creatures::AnimationMetadataDto>>)

};

#include OATPP_CODEGEN_END(DTO)
