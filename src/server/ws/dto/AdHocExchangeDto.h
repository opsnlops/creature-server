#pragma once

#include <chrono>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/AdHocExchange.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class AdHocExchangePartDto : public oatpp::DTO {
    DTO_INIT(AdHocExchangePartDto, DTO)
    DTO_FIELD(UInt32, index);
    DTO_FIELD(String, animation_id);
    DTO_FIELD(String, text);
    DTO_FIELD(UInt64, duration_ms);
};

class AdHocExchangeDto : public oatpp::DTO {
    DTO_INIT(AdHocExchangeDto, DTO)
    DTO_FIELD(String, session_id);
    DTO_FIELD(String, creature_id);
    DTO_FIELD(String, creature_name);
    DTO_FIELD(String, status);
    DTO_FIELD(String, title);
    DTO_FIELD(String, transcript);
    DTO_FIELD(UInt64, duration_ms);
    DTO_FIELD(String, created_at);
    DTO_FIELD(String, finished_at);
    DTO_FIELD(Vector<Object<AdHocExchangePartDto>>, parts);
};

class AdHocExchangeListDto : public oatpp::DTO {
    DTO_INIT(AdHocExchangeListDto, DTO)
    DTO_FIELD(UInt32, count);
    DTO_FIELD(Vector<Object<AdHocExchangeDto>>, items);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<AdHocExchangeDto> convertToDto(const AdHocExchange &exchange,
                                             const std::chrono::system_clock::time_point &createdAt);

} // namespace creatures
