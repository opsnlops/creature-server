
#include "Input.h"

#include <limits>

#include "model/JsonCodec.h"

namespace creatures {

namespace {

template <typename T> Result<Input> forwardError(const Result<T> &result) {
    return Result<Input>{result.getError().value()};
}

} // namespace

nlohmann::json inputToJson(const Input &input) {
    return {{"name", input.name}, {"slot", input.slot}, {"width", input.width}, {"joystick_axis", input.joystick_axis}};
}

Result<Input> inputFromJson(const nlohmann::json &json, std::string_view path) {
    auto fieldsResult = json_codec::rejectUnknownFields(json, path, {"name", "slot", "width", "joystick_axis"});
    if (!fieldsResult.isSuccess())
        return forwardError(fieldsResult);

    auto nameResult = json_codec::requiredString(json, path, "name", MAX_INPUT_NAME_BYTES);
    if (!nameResult.isSuccess())
        return forwardError(nameResult);

    auto slotResult = json_codec::requiredUnsigned<uint16_t>(json, path, "slot", std::numeric_limits<uint16_t>::max());
    if (!slotResult.isSuccess())
        return forwardError(slotResult);

    auto widthResult = json_codec::requiredUnsigned<uint8_t>(json, path, "width", std::numeric_limits<uint8_t>::max());
    if (!widthResult.isSuccess())
        return forwardError(widthResult);
    if (widthResult.getValue().value() == 0) {
        return json_codec::invalid<Input>(fmt::format("{}.width must be greater than zero", path));
    }

    auto joystickAxisResult =
        json_codec::requiredUnsigned<uint8_t>(json, path, "joystick_axis", std::numeric_limits<uint8_t>::max());
    if (!joystickAxisResult.isSuccess())
        return forwardError(joystickAxisResult);

    Input input;
    input.name = nameResult.getValue().value();
    input.slot = slotResult.getValue().value();
    input.width = widthResult.getValue().value();
    input.joystick_axis = joystickAxisResult.getValue().value();
    return Result<Input>{input};
}

} // namespace creatures
