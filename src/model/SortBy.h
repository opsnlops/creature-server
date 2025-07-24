
#pragma once

#include <nlohmann/json.hpp>

namespace creatures {

enum class SortBy { name = 0, number = 1 };

NLOHMANN_JSON_SERIALIZE_ENUM(SortBy, {{SortBy::name, "name"}, {SortBy::number, "number"}})

} // namespace creatures