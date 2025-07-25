
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures ::ws {

class StatusDto : public oatpp::DTO {

    DTO_INIT(StatusDto, DTO)

    DTO_FIELD_INFO(status) { info->description = "Short status text"; }
    DTO_FIELD(String, status);

    DTO_FIELD_INFO(code) { info->description = "Status code"; }
    DTO_FIELD(UInt16, code);

    DTO_FIELD_INFO(message) { info->description = "Verbose message"; }
    DTO_FIELD(String, message);
};
} // namespace creatures::ws
#include OATPP_CODEGEN_END(DTO)
