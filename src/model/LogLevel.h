
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/Types.hpp>

#include <unordered_map>
#include <string>

namespace creatures {

    enum class LogLevel {
        trace = 0,
        debug = 1,
        info = 2,
        warn = 3,
        error = 4,
        critical = 5,
        off = 6,
        unknown = 7
    };

} // namespace creatures
