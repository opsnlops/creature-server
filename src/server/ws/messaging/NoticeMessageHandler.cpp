
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
            // Print out the DTO for debugging
            appLogger->info("Decoding into a notice: {}", std::string(message));

            auto dto = objectMapper->readFromString<oatpp::Object<creatures::ws::NoticeMessageCommandDTO>>(message);
            if (dto) {
                Notice notice = convertFromDto(dto->payload.getPtr());

                // Just toss this to the logger, these are mostly for testing
                appLogger->info("A client would really like for us to know: {} at {}",
                                notice.message,
                                notice.timestamp);

            } else {
                appLogger->warn("unable to cast an incoming message to 'Notice'");
            }

        } catch (const std::bad_cast &e) {
            appLogger->warn("Error (std::bad_cast) while processing '{}' into a Notice message: {} ",
                            std::string(message), e.what());
        } catch (const std::exception &e) {
            appLogger->warn("Error (std::exception) while processing '{}' into a Notice message: {}",
                            std::string(message), e.what());
        } catch (...) {
            appLogger->warn("An unknown error happened while processing '{}' into a Notice message",
                            std::string(message));
        }

    }

}