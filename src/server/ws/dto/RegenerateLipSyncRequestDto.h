#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO) // NOLINT

namespace creatures::ws {

class RegenerateLipSyncRequestDto : public oatpp::DTO {
    DTO_INIT(RegenerateLipSyncRequestDto, DTO)

    DTO_FIELD_INFO(animation_id) {
        info->description = "Identifier of the animation to regenerate lip sync for";
    }
    DTO_FIELD(String, animation_id);
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO) // NOLINT

