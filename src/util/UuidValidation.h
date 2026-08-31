#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

namespace creatures {

/// Cheap RFC 4122 UUID shape check. Accepts the canonical 8-4-4-4-12 hex form,
/// case-insensitive. Does not validate version or variant bits.
inline bool isUuidShape(std::string_view value) {
    if (value.size() != 36)
        return false;
    constexpr std::size_t dashPositions[] = {8, 13, 18, 23};
    for (const auto position : dashPositions) {
        if (value[position] != '-')
            return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23)
            continue;
        if (!std::isxdigit(static_cast<unsigned char>(value[index])))
            return false;
    }
    return true;
}

} // namespace creatures
