

#include "model/Notice.h"
#include "model/JsonCodec.h"

namespace creatures {

namespace {

template <typename T> Result<Notice> forwardNoticeError(const Result<T> &result) {
    return Result<Notice>{result.getError().value()};
}

} // namespace

nlohmann::json noticeToJson(const Notice &notice) {
    return {{"timestamp", notice.timestamp}, {"message", notice.message}};
}

Result<Notice> noticeFromJson(const nlohmann::json &json, std::string_view path) {
    auto fields = json_codec::rejectUnknownFields(json, path, {"timestamp", "message"});
    if (!fields.isSuccess())
        return forwardNoticeError(fields);
    auto timestamp = json_codec::requiredString(json, path, "timestamp", MAX_NOTICE_TIMESTAMP_BYTES);
    auto message = json_codec::requiredString(json, path, "message", MAX_NOTICE_MESSAGE_BYTES);
    if (!timestamp.isSuccess())
        return forwardNoticeError(timestamp);
    if (!message.isSuccess())
        return forwardNoticeError(message);

    return Result<Notice>{Notice{timestamp.getValue().value(), message.getValue().value()}};
}

} // namespace creatures
