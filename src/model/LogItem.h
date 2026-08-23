
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "model/LogLevel.h"
#include "util/Result.h"

namespace creatures {

struct LogItem {
  public:
    std::string timestamp;
    LogLevel level;
    std::string message;
    std::string logger_name;
    uint32_t thread_id;
};

inline constexpr std::size_t MAX_LOG_TIMESTAMP_BYTES = 128;
inline constexpr std::size_t MAX_LOG_MESSAGE_BYTES = 64 * 1024;
inline constexpr std::size_t MAX_LOGGER_NAME_BYTES = 256;

nlohmann::json logItemToJson(const LogItem &logItem);
Result<LogItem> logItemFromJson(const nlohmann::json &json, std::string_view path = "log_item");

} // namespace creatures
