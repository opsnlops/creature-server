

#include <string>

#include <oatpp/core/Types.hpp>

#include "Notice.h"

namespace creatures {

Notice convertFromDto(const std::shared_ptr<NoticeDto> &noticeDto) {
    Notice notice;
    notice.timestamp = noticeDto->timestamp;
    notice.message = noticeDto->message;

    return notice;
}

oatpp::Object<NoticeDto> convertToDto(const Notice &notice) {
    auto noticeDto = NoticeDto::createShared();
    noticeDto->timestamp = notice.timestamp;
    noticeDto->message = notice.message;

    return noticeDto;
}

} // namespace creatures