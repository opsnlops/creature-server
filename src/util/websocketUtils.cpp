
#include <spdlog/spdlog.h>

#include <oatpp/core/Types.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "blockingconcurrentqueue.h"

#include "model/CacheInvalidation.h"
#include "server/config.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/ws/dto/JobCompleteDto.h"
#include "server/ws/dto/JobProgressDto.h"
#include "server/ws/dto/websocket/CacheInvalidationMessage.h"
#include "server/ws/dto/websocket/JobCompleteMessage.h"
#include "server/ws/dto/websocket/JobProgressMessage.h"
#include "server/ws/dto/websocket/MessageTypes.h"
#include "server/ws/dto/websocket/NoticeMessage.h"
#include "server/ws/dto/websocket/PlaylistStatusMessage.h"
#include "util/Result.h"
#include "util/helpers.h"
#include "util/websocketUtils.h"

#include "server/namespace-stuffs.h"

namespace creatures {

extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>>
    websocketOutgoingMessages;
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
        auto jsonMapper =
            oatpp::parser::json::mapping::ObjectMapper::createShared();

        std::string outgoingMessage = jsonMapper->writeToString(noticeMessage);
        debug("Outgoing notice to clients: {}", outgoingMessage);

        websocketOutgoingMessages->enqueue(outgoingMessage);
        return Result<bool>{true};
    } catch (const std::exception &e) {
        return Result<bool>{ServerError(ServerError::InternalError, e.what())};
    } catch (...) {
        return Result<bool>{ServerError(
            ServerError::InternalError,
            "broadcastNoticeToAllClients() caught an unknown exception")};
    }
}

/**
 * Broadcast a message to any client listening that they should invalidate a
 * certain type of cache
 *
 * @param type the `CacheType` that should be invalidated
 * @return true if the message was sent, ServerError if there was an issue
 */
Result<bool> broadcastCacheInvalidationToAllClients(const CacheType &type) {

    auto cacheTypeString = toString(type);
    info("broadcasting a '{}' cache invalidation to all clients",
         cacheTypeString);

    // Make sure this is a valid cache type
    if (type == CacheType::Unknown) {
        auto errorMessage = fmt::format("Cannot invalidate cache of type '{}'",
                                        cacheTypeString);
        warn(errorMessage);
        return Result<bool>{
            ServerError(ServerError::InvalidData, errorMessage)};
    }

    try {
        CacheInvalidation cacheInvalidation{};
        cacheInvalidation.cache_type = type;

        // Create the message to send with the command and payload
        auto invalidateMessage =
            oatpp::Object<ws::CacheInvalidationMessage>::createShared();
        invalidateMessage->command =
            toString(ws::MessageType::CacheInvalidation);
        invalidateMessage->payload = creatures::convertToDto(cacheInvalidation);

        // Make a JSON mapper
        auto jsonMapper =
            oatpp::parser::json::mapping::ObjectMapper::createShared();

        std::string outgoingMessage =
            jsonMapper->writeToString(invalidateMessage);
        debug("Outgoing cache invalidation to clients: {}", outgoingMessage);

        websocketOutgoingMessages->enqueue(outgoingMessage);
        return Result<bool>{true};
    } catch (const std::exception &e) {
        return Result<bool>{ServerError(ServerError::InternalError, e.what())};
    } catch (...) {
        return Result<bool>{ServerError(ServerError::InternalError,
                                        "broadcastCacheInvalidationToAllClients"
                                        "() caught an unknown exception")};
    }
}

void scheduleCacheInvalidationEvent(framenum_t frameOffset, CacheType type) {

    framenum_t eventTime = eventLoop->getCurrentFrameNumber() + frameOffset;

    auto invalidateEvent =
        std::make_shared<CacheInvalidateEvent>(eventTime, type);

    eventLoop->scheduleEvent(invalidateEvent);
    debug("cache invalidate message for the '{}' cache scheduled for frame {}",
          toString(type), eventTime);
}

Result<bool>
broadcastPlaylistStatusToAllClients(const PlaylistStatus &playlistStatus) {

    debug("broadcasting playlist status to all clients: {}",
          playlistStatus.playlist);

    try {

        // Create the message to send with the command and payload
        auto playlistStatusMessage =
            oatpp::Object<ws::PlaylistStatusMessage>::createShared();
        playlistStatusMessage->command =
            toString(ws::MessageType::PlaylistStatus);
        playlistStatusMessage->payload =
            creatures::convertToDto(playlistStatus);

        // Make a JSON mapper
        auto jsonMapper =
            oatpp::parser::json::mapping::ObjectMapper::createShared();

        std::string outgoingMessage =
            jsonMapper->writeToString(playlistStatusMessage);
        debug("Outgoing playlist update for clients: {}", outgoingMessage);

        websocketOutgoingMessages->enqueue(outgoingMessage);
        return Result<bool>{true};
    } catch (const std::exception &e) {
        return Result<bool>{ServerError(ServerError::InternalError, e.what())};
    } catch (...) {
        return Result<bool>{ServerError(ServerError::InternalError,
                                        "broadcastPlaylistStatusToAllClients() "
                                        "caught an unknown exception")};
    }
}

Result<bool> broadcastJobProgressToAllClients(const jobs::JobState &jobState) {

    debug("broadcasting job progress to all clients: job_id={}, progress={:.1f}%", jobState.jobId,
          jobState.progress * 100.0f);

    try {
        // Create the DTO
        auto progressDto = oatpp::Object<ws::JobProgressDto>::createShared();
        progressDto->job_id = jobState.jobId;
        progressDto->job_type = jobs::toString(jobState.jobType);
        progressDto->status = jobs::toString(jobState.status);
        progressDto->progress = jobState.progress;
        progressDto->details = jobState.details;

        // Create the message to send with the command and payload
        auto progressMessage = oatpp::Object<ws::JobProgressMessage>::createShared();
        progressMessage->command = toString(ws::MessageType::JobProgress);
        progressMessage->payload = progressDto;

        // Make a JSON mapper
        auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();

        std::string outgoingMessage = jsonMapper->writeToString(progressMessage);
        debug("Outgoing job progress for clients: {}", outgoingMessage);

        websocketOutgoingMessages->enqueue(outgoingMessage);
        return Result<bool>{true};
    } catch (const std::exception &e) {
        return Result<bool>{ServerError(ServerError::InternalError, e.what())};
    } catch (...) {
        return Result<bool>{
            ServerError(ServerError::InternalError, "broadcastJobProgressToAllClients() caught an unknown exception")};
    }
}

Result<bool> broadcastJobCompleteToAllClients(const jobs::JobState &jobState) {

    info("broadcasting job completion to all clients: job_id={}, status={}", jobState.jobId,
         jobs::toString(jobState.status));

    try {
        // Create the DTO
        auto completeDto = oatpp::Object<ws::JobCompleteDto>::createShared();
        completeDto->job_id = jobState.jobId;
        completeDto->job_type = jobs::toString(jobState.jobType);
        completeDto->status = jobs::toString(jobState.status);
        completeDto->result = jobState.result;
        completeDto->details = jobState.details;

        // Create the message to send with the command and payload
        auto completeMessage = oatpp::Object<ws::JobCompleteMessage>::createShared();
        completeMessage->command = toString(ws::MessageType::JobComplete);
        completeMessage->payload = completeDto;

        // Make a JSON mapper
        auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();

        std::string outgoingMessage = jsonMapper->writeToString(completeMessage);
        debug("Outgoing job completion for clients: {}", outgoingMessage);

        websocketOutgoingMessages->enqueue(outgoingMessage);
        return Result<bool>{true};
    } catch (const std::exception &e) {
        return Result<bool>{ServerError(ServerError::InternalError, e.what())};
    } catch (...) {
        return Result<bool>{
            ServerError(ServerError::InternalError, "broadcastJobCompleteToAllClients() caught an unknown exception")};
    }
}

} // namespace creatures
