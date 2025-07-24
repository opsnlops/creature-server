
#pragma once

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures {

struct Input {
    std::string name;
    uint16_t slot;
    uint8_t width;
    uint8_t joystick_axis;
};

#include OATPP_CODEGEN_BEGIN(DTO)

class InputDto : public oatpp::DTO {

    DTO_INIT(InputDto, DTO /* extends */)

    DTO_FIELD_INFO(name) { info->description = "The name of the input"; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(slot) {
        info->description = "Which slot this input maps to in the e1.31 packet "
                            "for this creature";
    }
    DTO_FIELD(UInt16, slot);

    DTO_FIELD_INFO(width) {
        info->description = "How many consecutive slots this input uses in the "
                            "e1.31 packet for this creature";
    }
    DTO_FIELD(UInt8, width);

    DTO_FIELD_INFO(joystick_axis) {
        info->description = "When recording or streaming, which axis on the "
                            "joystick should this input be mapped to";
    }
    DTO_FIELD(UInt8, joystick_axis);
};

#include OATPP_CODEGEN_END(DTO)

std::shared_ptr<InputDto> convertToDto(const InputDto &input);
InputDto convertFromDto(const std::shared_ptr<InputDto> &inputDto);

} // namespace creatures