
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include <string>
#include <unordered_map>

namespace creatures {

enum class LogLevel { trace = 0, debug = 1, info = 2, warn = 3, error = 4, critical = 5, off = 6, unknown = 7 };

constexpr std::string toString(LogLevel type) {
    switch (type) {
    case LogLevel::trace:
        return "trace";
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warn:
        return "warning";
    case LogLevel::error:
        return "error";
    case LogLevel::critical:
        return "critical";
    case LogLevel::off:
        return "off";

    default:
        return "unknown";
    }
}

constexpr LogLevel fromString(const std::string &str) {
    if (str == "trace")
        return LogLevel::trace;
    else if (str == "debug")
        return LogLevel::debug;
    else if (str == "info")
        return LogLevel::info;
    else if (str == "warning")
        return LogLevel::warn;
    else if (str == "error")
        return LogLevel::error;
    else if (str == "critical")
        return LogLevel::critical;
    else if (str == "off")
        return LogLevel::off;
    else
        throw std::invalid_argument("Invalid LogLevel string: " + str);
}

} // namespace creatures
