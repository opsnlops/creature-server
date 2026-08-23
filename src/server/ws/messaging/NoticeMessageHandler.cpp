
#include "spdlog/spdlog.h"
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "model/Notice.h"

#include "NoticeMessageCommandDTO.h"
#include "NoticeMessageHandler.h"

namespace creatures ::ws {

void NoticeMessageHandler::processMessage(const oatpp::String &message) {

    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    try {

        auto objectMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
        objectMapper->getDeserializer()->getConfig()->allowUnknownFields = false;
        appLogger->debug("Decoding inbound Notice ({} bytes)", message->size());

        auto dto = objectMapper->readFromString<oatpp::Object<creatures::ws::NoticeMessageCommandDTO>>(message);
        if (dto && dto->payload) {
            const auto parsedNotice = noticeFromJson(noticeToJson(convertFromDto(dto->payload)));
            if (!parsedNotice.isSuccess()) {
                appLogger->warn("Rejected invalid inbound Notice: {}", parsedNotice.getError()->getMessage());
                return;
            }
            const auto notice = parsedNotice.getValue().value();

            // Just toss this to the logger, these are mostly for testing
            appLogger->info("Accepted inbound Notice ({} message bytes)", notice.message.size());

        } else {
            appLogger->warn("unable to decode an incoming Notice payload");
        }

    } catch (const std::bad_cast &e) {
        appLogger->warn("std::bad_cast while processing inbound Notice: {}", e.what());
    } catch (const std::exception &e) {
        appLogger->warn("Exception while processing inbound Notice: {}", e.what());
    } catch (...) {
        appLogger->warn("Unknown exception while processing inbound Notice");
    }
}

} // namespace creatures::ws
