
#pragma once

#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "LogLevel.h"

namespace creatures {

struct LogItem {
  public:
    std::string timestamp;
    LogLevel level;
    std::string message;
    std::string logger_name;
    uint32_t thread_id;
};

#include OATPP_CODEGEN_BEGIN(DTO)

class LogItemDto : public oatpp::DTO {

    DTO_INIT(LogItemDto, DTO /* extends */)

    DTO_FIELD(String, timestamp);
    DTO_FIELD(String, level);
    DTO_FIELD(String, message);
    DTO_FIELD(String, logger_name);
    DTO_FIELD(UInt32, thread_id);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<LogItemDto> convertToDto(const LogItem &logItem);
LogItem convertFromDto(const std::shared_ptr<LogItemDto> &logItemDto);

} // namespace creatures
