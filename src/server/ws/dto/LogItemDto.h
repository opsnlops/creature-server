#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/LogItem.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class LogItemDto : public oatpp::DTO {
    DTO_INIT(LogItemDto, DTO)

    DTO_FIELD(String, timestamp);
    DTO_FIELD(String, level);
    DTO_FIELD(String, message);
    DTO_FIELD(String, logger_name);
    DTO_FIELD(UInt32, thread_id);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<LogItemDto> convertToDto(const LogItem &logItem);
LogItem convertFromDto(const oatpp::Object<LogItemDto> &logItemDto);

} // namespace creatures
