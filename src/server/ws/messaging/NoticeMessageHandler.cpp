
#include "model/Notice.h"
#include "util/ObservabilityManager.h"

#include "NoticeMessageHandler.h"

namespace creatures ::ws {

bool NoticeMessageHandler::processMessage(const nlohmann::json &payload, std::string_view message,
                                          std::string_view command, std::shared_ptr<SamplingSpan> messageSpan) {

    static_cast<void>(message);
    static_cast<void>(command);

    try {
        const auto parsedNotice = noticeFromJson(payload);
        if (!parsedNotice.isSuccess()) {
            const auto errorMessage = parsedNotice.getError()->getMessage();
            logger_->warn("Rejected invalid inbound Notice: {}", errorMessage);
            if (messageSpan) {
                messageSpan->setError(errorMessage);
                messageSpan->setAttribute("websocket.rejection.stage", "payload");
                messageSpan->setAttribute("error.type", "InvalidNotice");
                messageSpan->setAttribute("error.message", errorMessage);
                messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            }
            return false;
        }
        const auto notice = parsedNotice.getValue().value();

        // Just toss this to the logger, these are mostly for testing.
        logger_->info("Accepted inbound Notice ({} message bytes)", notice.message.size());
        return true;

    } catch (const std::exception &e) {
        const auto errorMessage = fmt::format("Exception while processing inbound Notice: {}", e.what());
        logger_->warn(errorMessage);
        if (messageSpan) {
            messageSpan->recordException(e);
            messageSpan->setError(errorMessage);
            messageSpan->setAttribute("error.type", "std::exception");
            messageSpan->setAttribute("error.message", errorMessage);
            messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
        }
    } catch (...) {
        logger_->warn("Unknown exception while processing inbound Notice");
        if (messageSpan) {
            messageSpan->setError("Unknown error while processing inbound Notice");
            messageSpan->setAttribute("error.type", "unknown");
            messageSpan->setAttribute("error.message", "Unknown error while processing inbound Notice");
            messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
        }
    }
    return false;
}

} // namespace creatures::ws
