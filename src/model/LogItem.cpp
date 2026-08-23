

#include "model/LogItem.h"
#include "model/JsonCodec.h"

namespace creatures {

namespace {

template <typename T> Result<LogItem> forwardLogItemError(const Result<T> &result) {
    return Result<LogItem>{result.getError().value()};
}

} // namespace

nlohmann::json logItemToJson(const LogItem &logItem) {
    return {{"timestamp", logItem.timestamp},
            {"level", toString(logItem.level)},
            {"message", logItem.message},
            {"logger_name", logItem.logger_name},
            {"thread_id", logItem.thread_id}};
}

Result<LogItem> logItemFromJson(const nlohmann::json &json, std::string_view path) {
    auto fields =
        json_codec::rejectUnknownFields(json, path, {"timestamp", "level", "message", "logger_name", "thread_id"});
    if (!fields.isSuccess())
        return forwardLogItemError(fields);
    auto timestamp = json_codec::requiredString(json, path, "timestamp", MAX_LOG_TIMESTAMP_BYTES);
    auto level = json_codec::requiredString(json, path, "level", 16);
    auto message = json_codec::requiredString(json, path, "message", MAX_LOG_MESSAGE_BYTES);
    auto loggerName = json_codec::requiredString(json, path, "logger_name", MAX_LOGGER_NAME_BYTES);
    auto threadId =
        json_codec::requiredUnsigned<uint32_t>(json, path, "thread_id", std::numeric_limits<uint32_t>::max());
    if (!timestamp.isSuccess())
        return forwardLogItemError(timestamp);
    if (!level.isSuccess())
        return forwardLogItemError(level);
    if (!message.isSuccess())
        return forwardLogItemError(message);
    if (!loggerName.isSuccess())
        return forwardLogItemError(loggerName);
    if (!threadId.isSuccess())
        return forwardLogItemError(threadId);

    try {
        return Result<LogItem>{LogItem{timestamp.getValue().value(), fromString(level.getValue().value()),
                                       message.getValue().value(), loggerName.getValue().value(),
                                       threadId.getValue().value()}};
    } catch (const std::invalid_argument &) {
        return json_codec::invalid<LogItem>(fmt::format("{}.level is not recognized", path));
    }
}

} // namespace creatures
#include <limits>
