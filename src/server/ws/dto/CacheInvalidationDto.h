#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/CacheInvalidation.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class CacheInvalidationDto : public oatpp::DTO {
    DTO_INIT(CacheInvalidationDto, DTO)

    DTO_FIELD_INFO(cache_type) { info->description = "A string representation of the cache to invalidate"; }
    DTO_FIELD(String, cache_type);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<CacheInvalidationDto> convertToDto(const CacheInvalidation &cacheInvalidation);
CacheInvalidation convertFromDto(const oatpp::Object<CacheInvalidationDto> &cacheInvalidationDto);

} // namespace creatures
