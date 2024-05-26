
#include <string>

#include <oatpp/core/Types.hpp>

#include "Input.h"

namespace creatures {


    Input convertFromDto(const std::shared_ptr<Input> &inputDto) {
        Input input;
        input.name = inputDto->name;
        input.slot = inputDto->slot;
        input.width = inputDto->width;
        input.joystick_axis = inputDto->joystick_axis;

        return input;
    }


    oatpp::Object<InputDto> convertToDto(const Input &input) {
        auto inputDto = InputDto::createShared();
        inputDto->name = input.name;
        inputDto->slot = input.slot;
        inputDto->width = input.width;
        inputDto->joystick_axis = input.joystick_axis;

        return inputDto;
    }

}