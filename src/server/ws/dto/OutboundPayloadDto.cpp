#include "server/ws/dto/NoticeDto.h"

namespace creatures {

oatpp::Object<NoticeDto> convertToDto(const Notice &notice) {
    auto dto = NoticeDto::createShared();
    dto->timestamp = notice.timestamp;
    dto->message = notice.message;
    return dto;
}

Notice convertFromDto(const oatpp::Object<NoticeDto> &dto) { return Notice{dto->timestamp, dto->message}; }

} // namespace creatures
