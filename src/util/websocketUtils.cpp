
#include <spdlog/spdlog.h>

#include "blockingconcurrentqueue.h"

#include "api/WebSocketEnvelope.h"
#include "model/CacheInvalidation.h"
#include "model/Notice.h"
#include "server/config.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/jobs/JobPayload.h"
#include "server/ws/dto/websocket/MessageTypes.h"
#include "util/Result.h"
#include "util/helpers.h"
#include "util/websocketUtils.h"

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

    if (!websocketOutgoingMessages) {
        return Result<bool>{ServerError(ServerError::InternalError, "Websocket queue unavailable")};
    }

    try {
        // Create the actual Notice object
        Notice notice;
        notice.timestamp = getCurrentTimeISO8601();
        notice.message = message;

        const std::string outgoingMessage =
            api::serializeWebSocketEnvelope(toString(ws::MessageType::Notice), noticeToJson(notice));
        debug("Outgoing notice to clients: {}", outgoingMessage);

        websocketOutgoingMessages->enqueue(outgoingMessage);
        return Result<bool>{true};
    } catch (const std::exception &e) {
        return Result<bool>{ServerError(ServerError::InternalError, e.what())};
    } catch (...) {
        return Result<bool>{
            ServerError(ServerError::InternalError, "broadcastNoticeToAllClients() caught an unknown exception")};
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
    info("broadcasting a '{}' cache invalidation to all clients", cacheTypeString);

    if (!websocketOutgoingMessages) {
        auto errorMessage = "Websocket queue unavailable";
        warn(errorMessage);
        return Result<bool>{ServerError(ServerError::InternalError, errorMessage)};
    }

    // Make sure this is a valid cache type
    if (type == CacheType::Unknown) {

        auto errorMessage = fmt::format("Cannot invalidate cache of type '{}'", cacheTypeString);
        warn(errorMessage);
        return Result<bool>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    try {
        CacheInvalidation cacheInvalidation{};
        cacheInvalidation.cache_type = type;

        const std::string outgoingMessage = api::serializeWebSocketEnvelope(
            toString(ws::MessageType::CacheInvalidation), cacheInvalidationToJson(cacheInvalidation));
        debug("Outgoing cache invalidation to clients: {}", outgoingMessage);

        websocketOutgoingMessages->enqueue(outgoingMessage);
        return Result<bool>{true};
    } catch (const std::exception &e) {
        return Result<bool>{ServerError(ServerError::InternalError, e.what())};
    } catch (...) {
        return Result<bool>{ServerError(ServerError::InternalError, "broadcastCacheInvalidationToAllClients"
                                                                    "() caught an unknown exception")};
    }
}

void scheduleCacheInvalidationEvent(framenum_t frameOffset, CacheType type) {
    if (!eventLoop) {
        warn("scheduleCacheInvalidationEvent skipped: event loop unavailable");
        return;
    }

    framenum_t eventTime = eventLoop->getCurrentFrameNumber() + frameOffset;

    auto invalidateEvent = std::make_shared<CacheInvalidateEvent>(eventTime, type);

    eventLoop->scheduleEvent(invalidateEvent);
    debug("cache invalidate message for the '{}' cache scheduled for frame {}", toString(type), eventTime);
}

Result<bool> broadcastPlaylistStatusToAllClients(const PlaylistStatus &playlistStatus) {

    debug("broadcasting playlist status to all clients: {}", playlistStatus.playlist);

    if (!websocketOutgoingMessages) {
        return Result<bool>{ServerError(ServerError::InternalError, "Websocket queue unavailable")};
    }

    try {

        const std::string outgoingMessage = api::serializeWebSocketEnvelope(toString(ws::MessageType::PlaylistStatus),
                                                                            playlistStatusToJson(playlistStatus));
        debug("Outgoing playlist update for clients: {}", outgoingMessage);

        websocketOutgoingMessages->enqueue(outgoingMessage);
        return Result<bool>{true};
    } catch (const std::exception &e) {
        return Result<bool>{ServerError(ServerError::InternalError, e.what())};
    } catch (...) {
        return Result<bool>{ServerError(ServerError::InternalError, "broadcastPlaylistStatusToAllClients() "
                                                                    "caught an unknown exception")};
    }
}

Result<bool> broadcastJobProgressToAllClients(const jobs::JobState &jobState) {

    debug("broadcasting job progress to all clients: job_id={}, progress={:.1f}%", jobState.jobId,
          jobState.progress * 100.0f);

    if (!websocketOutgoingMessages) {
        return Result<bool>{ServerError(ServerError::InternalError, "Websocket queue unavailable")};
    }

    try {
        const std::string outgoingMessage =
            api::serializeWebSocketEnvelope(toString(ws::MessageType::JobProgress), jobs::jobProgressToJson(jobState));
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

    if (!websocketOutgoingMessages) {
        return Result<bool>{ServerError(ServerError::InternalError, "Websocket queue unavailable")};
    }

    try {
        const std::string outgoingMessage =
            api::serializeWebSocketEnvelope(toString(ws::MessageType::JobComplete), jobs::jobCompleteToJson(jobState));
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
