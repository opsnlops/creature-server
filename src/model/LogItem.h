
#pragma once

#include <string>
#include <chrono>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "LogLevel.h"


namespace creatures {

    struct LogItem {
    public:

        LogLevel level;
        std::string message;
        std::string logger_name;
        uint32_t thread_id;

    };

#include OATPP_CODEGEN_BEGIN(DTO)

class LogItemDTO : public oatpp::DTO {

    DTO_INIT(LogItemDTO, DTO /* extends */);

    DTO_FIELD(String, id);
    DTO_FIELD(String, creature_id);
    DTO_FIELD(String, animation_id);
    DTO_FIELD(List<String>, frames);

};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures
