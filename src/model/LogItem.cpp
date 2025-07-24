

#include <string>

#include <oatpp/core/Types.hpp>

#include "LogItem.h"

namespace creatures {

LogItem convertFromDto(const std::shared_ptr<LogItemDto> &logItemDto) {
    LogItem logItem;
    logItem.timestamp = logItemDto->timestamp;
    logItem.level = fromString(logItemDto->level);
    logItem.message = logItemDto->message;
    logItem.logger_name = logItemDto->logger_name;
    logItem.thread_id = logItemDto->thread_id;

    return logItem;
}

oatpp::Object<LogItemDto> convertToDto(const LogItem &logItem) {
    auto logItemDto = LogItemDto::createShared();
    logItemDto->timestamp = logItem.timestamp;
    logItemDto->level = toString(logItem.level);
    logItemDto->message = logItem.message;
    logItemDto->logger_name = logItem.logger_name;
    logItemDto->thread_id = logItem.thread_id;

    return logItemDto;
}

} // namespace creatures