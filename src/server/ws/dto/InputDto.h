#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/Input.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class InputDto : public oatpp::DTO {
    DTO_INIT(InputDto, DTO)

    DTO_FIELD_INFO(name) { info->description = "The name of the input"; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(slot) { info->description = "Which slot this input maps to in the E1.31 packet for this creature"; }
    DTO_FIELD(UInt16, slot);

    DTO_FIELD_INFO(width) {
        info->description = "How many consecutive slots this input uses in the E1.31 packet for this creature";
    }
    DTO_FIELD(UInt8, width);

    DTO_FIELD_INFO(joystick_axis) {
        info->description = "When recording or streaming, which joystick axis maps to this input";
    }
    DTO_FIELD(UInt8, joystick_axis);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<InputDto> convertToDto(const Input &input);
Input convertFromDto(const oatpp::Object<InputDto> &inputDto);

} // namespace creatures
