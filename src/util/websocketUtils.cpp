
#include <spdlog/spdlog.h>

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/Types.hpp>

#include "blockingconcurrentqueue.h"


#include "server/ws/dto/websocket/MessageTypes.h"
#include "server/ws/dto/websocket/NoticeMessage.h"

#include "util/helpers.h"
#include "util/websocketUtils.h"
#include "util/Result.h"



#include "server/namespace-stuffs.h"

namespace creatures {

    extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;


    /**
     * Broadcast out a message to all clients that are currently connected
     *
     * @param message the message to send
     * @return true is the message was sent, ServerError if there was an issue
     */
    Result<bool> broadcastNoticeToAllClients(const std::string &message) {

        info("broadcasting notice to all clients: {}", message);

        try {
            // Create the actual Notice object
            Notice notice;
            notice.timestamp = getCurrentTimeISO8601();
            notice.message = message;

            // Create the message to send with the command and payload
            auto noticeMessage = oatpp::Object<ws::NoticeMessage>::createShared();
            noticeMessage->command = toString(ws::MessageType::Notice);
            noticeMessage->payload = creatures::convertToDto(notice);

            // Make a JSON mapper
            auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();

            std::string outgoingMessage = jsonMapper->writeToString(noticeMessage);
            debug("Outgoing notice to clients: {}", outgoingMessage);

            websocketOutgoingMessages->enqueue(outgoingMessage);
            return Result<bool>{true};
        }
        catch (const std::exception &e) {
            return Result<bool>{ServerError(ServerError::InternalError, e.what())};
        }
        catch (...) {
            return Result<bool>{ServerError(ServerError::InternalError,
                                            "broadcastNoticeToAllClients() caught an unknown exception")};
        }

    }

}