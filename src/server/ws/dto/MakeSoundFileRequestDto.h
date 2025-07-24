
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures ::ws {

class MakeSoundFileRequestDto : public oatpp::DTO {

    DTO_INIT(MakeSoundFileRequestDto, DTO)

    DTO_FIELD_INFO(creature_id) {
        info->description = "The creature ID to make a sound file for";
        info->required = true;
    }
    DTO_FIELD(String, creature_id);

    DTO_FIELD_INFO(title) {
        info->description = "The title of the sound file. Used for metadata only.";
        info->required = false;
    }
    DTO_FIELD(String, title);

    DTO_FIELD_INFO(text) {
        info->description = "What to make the Creature say";
        info->required = true;
    }
    DTO_FIELD(String, text);
};
} // namespace creatures::ws
#include OATPP_CODEGEN_END(DTO)
