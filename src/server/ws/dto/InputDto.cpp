#include "server/ws/dto/InputDto.h"

namespace creatures {

oatpp::Object<InputDto> convertToDto(const Input &input) {
    auto dto = InputDto::createShared();
    dto->name = input.name;
    dto->slot = input.slot;
    dto->width = input.width;
    dto->joystick_axis = input.joystick_axis;
    return dto;
}

Input convertFromDto(const oatpp::Object<InputDto> &inputDto) {
    Input input;
    input.name = inputDto->name;
    input.slot = inputDto->slot;
    input.width = inputDto->width;
    input.joystick_axis = inputDto->joystick_axis;
    return input;
}

} // namespace creatures
