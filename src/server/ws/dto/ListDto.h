
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/Types.hpp>

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "model/Creature.h"
#include "model/Sound.h"


#include OATPP_CODEGEN_BEGIN(DTO)

template<class T>
class ListDto : public oatpp::DTO {

    DTO_INIT(ListDto, DTO)

    DTO_FIELD(UInt32, count);
    DTO_FIELD(Vector<T>, items);

};


class AnimationsListDto : public ListDto<oatpp::Object<creatures::AnimationMetadataDto>> {

    DTO_INIT(AnimationsListDto, ListDto<oatpp::Object<creatures::AnimationMetadataDto>>)

};

class CreaturesListDto : public ListDto<oatpp::Object<creatures::CreatureDto>> {

DTO_INIT(CreaturesListDto, ListDto<oatpp::Object<creatures::CreatureDto>>)

};


class SoundsListDto : public ListDto<oatpp::Object<creatures::SoundDto>> {

    DTO_INIT(SoundsListDto, ListDto<oatpp::Object<creatures::SoundDto>>)

};


#include OATPP_CODEGEN_END(DTO)
