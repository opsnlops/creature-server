#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace creatures::util {

[[nodiscard]] std::string sha256Hex(std::span<const uint8_t> bytes);
[[nodiscard]] std::string sha256Hex(const std::string &text);

} // namespace creatures::util
