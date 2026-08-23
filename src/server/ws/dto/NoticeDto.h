#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/Notice.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class NoticeDto : public oatpp::DTO {
    DTO_INIT(NoticeDto, DTO)

    DTO_FIELD_INFO(timestamp) {
        info->description = "The time this message was sent";
        info->required = true;
    }
    DTO_FIELD(String, timestamp);

    DTO_FIELD_INFO(message) {
        info->description = "The message the server wants us to know";
        info->required = true;
    }
    DTO_FIELD(String, message);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<NoticeDto> convertToDto(const Notice &notice);
Notice convertFromDto(const oatpp::Object<NoticeDto> &noticeDto);

} // namespace creatures
