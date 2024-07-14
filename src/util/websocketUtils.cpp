
#include <spdlog/spdlog.h>

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/Types.hpp>

#include "blockingconcurrentqueue.h"

#include "model/CacheInvalidation.h"
#include "server/config.h"
#include "server/eventloop/events/types.h"
#include "server/eventloop/eventloop.h"
#include "server/ws/dto/websocket/CacheInvalidationMessage.h"
#include "server/ws/dto/websocket/MessageTypes.h"
#include "server/ws/dto/websocket/NoticeMessage.h"
#include "util/helpers.h"
#include "util/websocketUtils.h"
#include "util/Result.h"


#include "server/namespace-stuffs.h"

namespace creatures {

    extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;
    extern std::shared_ptr<EventLoop> eventLoop;

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

    /**
     * Broadcast a message to any client listening that they should invalidate a certain
     * type of cache
     *
     * @param type the `CacheType` that should be invalidated
     * @return true if the message was sent, ServerError if there was an issue
     */
    Result<bool> broadcastCacheInvalidationToAllClients(const CacheType &type) {

        auto cacheTypeString = toString(type);
        info("broadcasting a '{}' cache invalidation to all clients", cacheTypeString);

        // Make sure this is a valid cache type
        if(type == CacheType::Unknown) {
            auto errorMessage = fmt::format("Cannot invalidate cache of type '{}'", cacheTypeString);
            warn(errorMessage);
            return Result<bool>{ServerError(ServerError::InvalidData, errorMessage)};
        }

        try {
            CacheInvalidation cacheInvalidation{};
            cacheInvalidation.cacheType = type;

             // Create the message to send with the command and payload
            auto invalidateMessage = oatpp::Object<ws::CacheInvalidationMessage>::createShared();
            invalidateMessage->command = toString(ws::MessageType::CacheInvalidation);
            invalidateMessage->payload = creatures::convertToDto(cacheInvalidation);

            // Make a JSON mapper
            auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();

            std::string outgoingMessage = jsonMapper->writeToString(invalidateMessage);
            debug("Outgoing cache invalidation to clients: {}", outgoingMessage);

            websocketOutgoingMessages->enqueue(outgoingMessage);
            return Result<bool>{true};
        }
        catch (const std::exception &e) {
            return Result<bool>{ServerError(ServerError::InternalError, e.what())};
        }
        catch (...) {
            return Result<bool>{ServerError(ServerError::InternalError,
                                            "broadcastCacheInvalidationToAllClients() caught an unknown exception")};
        }

    }


    void scheduleCacheInvalidationEvent(framenum_t frameOffset, CacheType type) {

        framenum_t eventTime = eventLoop->getCurrentFrameNumber() + frameOffset;

        auto invalidateEvent = std::make_shared<CacheInvalidateEvent>(eventTime, type);

        eventLoop->scheduleEvent(invalidateEvent);
        debug("cache invalidate message for the '{}' cache scheduled for frame {}", toString(type), eventTime);
    }
}