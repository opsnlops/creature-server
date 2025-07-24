
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures ::ws {

class SimpleResponseDto : public oatpp::DTO {

    DTO_INIT(SimpleResponseDto, DTO)

    DTO_FIELD_INFO(message) { info->description = "A message for you to know!"; }
    DTO_FIELD(String, message);
};
} // namespace creatures::ws
#include OATPP_CODEGEN_END(DTO)
