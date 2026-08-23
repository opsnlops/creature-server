
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "util/Result.h"

namespace creatures {

struct Input {
    std::string name;
    uint16_t slot{0};
    uint8_t width{0};
    uint8_t joystick_axis{0};

    bool operator==(const Input &) const = default;
};

inline constexpr std::size_t MAX_INPUT_NAME_BYTES = 128;

nlohmann::json inputToJson(const Input &input);
Result<Input> inputFromJson(const nlohmann::json &json, std::string_view path = "input");

} // namespace creatures
