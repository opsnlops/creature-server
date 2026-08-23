
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "util/Result.h"

namespace creatures {

struct Notice {

    /**
     * The time that this message was sent
     */
    std::string timestamp;

    /**
     * The actual message itself
     */
    std::string message;
};

inline constexpr std::size_t MAX_NOTICE_TIMESTAMP_BYTES = 128;
inline constexpr std::size_t MAX_NOTICE_MESSAGE_BYTES = 64 * 1024;

nlohmann::json noticeToJson(const Notice &notice);
Result<Notice> noticeFromJson(const nlohmann::json &json, std::string_view path = "notice");

} // namespace creatures
