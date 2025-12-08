#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

class IdleToggleDto : public oatpp::DTO {
    DTO_INIT(IdleToggleDto, DTO)

    DTO_FIELD_INFO(enabled) { info->description = "Whether idle should be enabled (true) or disabled (false)"; }
    DTO_FIELD(Boolean, enabled);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
