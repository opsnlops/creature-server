
#pragma once

#include <nlohmann/json.hpp>

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

    NLOHMANN_JSON_SERIALIZE_ENUM( LogLevel, {
        {LogLevel::trace, "trace"},
        {LogLevel::debug, "debug"},
        {LogLevel::info, "info"},
        {LogLevel::warn, "warn"},
        {LogLevel::error, "error"},
        {LogLevel::critical, "critical"},
        {LogLevel::off, "off"},
        {LogLevel::unknown, "unknown"}
    })

} // namespace creatures