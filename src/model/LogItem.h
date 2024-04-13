
#pragma once

#include <string>
#include <chrono>

#include <nlohmann/json.hpp>

#include "LogLevel.h"


namespace creatures {

    struct LogItem {
    public:

        LogLevel level;
        std::string message;
        std::string logger_name;
        uint32_t thread_id;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(LogItem, level, message, logger_name, thread_id)

    };

} // namespace creatures
