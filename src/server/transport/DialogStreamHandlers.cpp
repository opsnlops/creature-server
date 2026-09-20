#include "server/transport/DialogStreamHandlers.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "api/DialogStreamContracts.h"
#include "api/JsonResponse.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/transport/HandlerSupport.h"
#include "server/voice/StreamingAdHocSession.h"
#include "util/ObservabilityManager.h"
#include "util/helpers.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::transport {
namespace {} // namespace

PreparedResponse startDialogStream(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::config || !creatures::db)
        return errorStatus(500, "Streaming dialog unavailable: server dependencies missing", span);

    const auto parsed = parseBody(body, "dialog.stream.start", "dialog stream start request",
                                  api::dialogStreamStartRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    if (span) {
        span->setAttribute("creature.id", canonicalUuid(request.creatureIds.front()));
        span->setAttribute("session.participants", static_cast<int64_t>(request.creatureIds.size()));
        span->setAttribute("stage.id", canonicalUuid(request.stageId));
    }

    voice::StreamingSessionConfig sessionConfig;
    sessionConfig.creatureIds = request.creatureIds;
    sessionConfig.stageId = request.stageId;
    sessionConfig.resumePlaylist = request.resumePlaylist;
    sessionConfig.dialog = true;
    auto &manager = voice::StreamingAdHocSessionManager::instance();
    const auto created = manager.createSession(std::move(sessionConfig), span);
    if (!created.isSuccess())
        return serverErrorStatus(created.getError().value(), span);
    const auto session = created.getValue().value();
    const auto started = session->start();
    if (!started.isSuccess()) {
        manager.removeSession(session->getSessionId());
        return serverErrorStatus(started.getError().value(), span);
    }

    const api::DialogStreamStartResponse response{session->getSessionId(), "started",
                                                  "Session started. Send turns via /turn, then call /finish.",
                                                  request.creatureIds, request.stageId};
    if (span) {
        span->setAttribute("session.id", session->getSessionId());
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::dialogStreamStartResponseToJson(response).dump());
}

PreparedResponse addDialogStreamTurn(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed =
        parseBody(body, "dialog.stream.turn", "dialog stream turn request", api::dialogStreamTurnRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    if (span) {
        span->setAttribute("session.id", canonicalUuid(request.sessionId));
        span->setAttribute("creature.id", canonicalUuid(request.creatureId));
        span->setAttribute("text.length", static_cast<int64_t>(request.text.size()));
    }
    auto session = voice::StreamingAdHocSessionManager::instance().getSession(request.sessionId);
    if (!session)
        return errorStatus(404, "Session not found", span);
    const auto added = session->addTurn(request.creatureId, request.text, span);
    if (!added.isSuccess())
        return serverErrorStatus(added.getError().value(), span);
    const api::DialogStreamTurnResponse response{request.sessionId, "ok", session->getChunksReceived()};
    if (span) {
        span->setAttribute("text.chunks.received", static_cast<int64_t>(session->getChunksReceived()));
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::dialogStreamTurnResponseToJson(response).dump());
}

PreparedResponse finishDialogStream(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed = parseBody(body, "dialog.stream.finish", "dialog stream finish request",
                                  api::dialogStreamFinishRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    if (span)
        span->setAttribute("session.id", canonicalUuid(request.sessionId));
    auto &manager = voice::StreamingAdHocSessionManager::instance();
    auto session = manager.getSession(request.sessionId);
    if (!session)
        return errorStatus(404, "Session not found", span);
    const auto completed = session->finish(span);
    if (!completed.isSuccess())
        return serverErrorStatus(completed.getError().value(), span);
    manager.removeSession(request.sessionId);
    const auto summary = completed.getValue().value();
    const api::DialogStreamFinishResponse response{request.sessionId,
                                                   "completed",
                                                   "Dialog played and stitched",
                                                   summary.exchangeAnimationId,
                                                   summary.lastAnimationId,
                                                   summary.partsRendered > 0,
                                                   summary.exchangeStatus,
                                                   summary.partsRendered,
                                                   summary.partsTotal};
    if (span) {
        span->setAttribute("exchange.status", summary.exchangeStatus);
        span->setAttribute("exchange.parts.rendered", static_cast<int64_t>(summary.partsRendered));
        span->setAttribute("exchange.parts.total", static_cast<int64_t>(summary.partsTotal));
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::dialogStreamFinishResponseToJson(response).dump());
}

PreparedResponse startStreamingAdHoc(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::config || !creatures::db)
        return errorStatus(500, "Streaming ad-hoc speech unavailable: server dependencies missing", span);
    const auto parsed = parseBody(body, "ad-hoc.stream.start", "streaming ad-hoc start request",
                                  api::streamingAdHocStartRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    if (span) {
        span->setAttribute("creature.id", canonicalUuid(request.creatureId));
        span->setAttribute("playback.resume.playlist", request.resumePlaylist);
    }
    auto &manager = voice::StreamingAdHocSessionManager::instance();
    const auto created = manager.createSession(request.creatureId, request.resumePlaylist, span);
    if (!created.isSuccess())
        return serverErrorStatus(created.getError().value(), span);
    const auto session = created.getValue().value();
    const auto started = session->start();
    if (!started.isSuccess()) {
        manager.removeSession(session->getSessionId());
        return serverErrorStatus(started.getError().value(), span);
    }
    const api::StreamingAdHocStartResponse response{session->getSessionId(), "started",
                                                    "Session started. Send text chunks via /text, then call /finish."};
    if (span) {
        span->setAttribute("session.id", session->getSessionId());
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::streamingAdHocStartResponseToJson(response).dump());
}

PreparedResponse addStreamingAdHocText(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed = parseBody(body, "ad-hoc.stream.text", "streaming ad-hoc text request",
                                  api::streamingAdHocTextRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    if (span) {
        span->setAttribute("session.id", canonicalUuid(request.sessionId));
        span->setAttribute("text.length", static_cast<int64_t>(request.text.size()));
    }
    auto session = voice::StreamingAdHocSessionManager::instance().getSession(request.sessionId);
    if (!session)
        return errorStatus(404, "Session not found", span);
    const auto added = session->addText(request.text, span);
    if (!added.isSuccess())
        return serverErrorStatus(added.getError().value(), span);
    const api::StreamingAdHocTextResponse response{request.sessionId, "ok", session->getChunksReceived()};
    if (span) {
        span->setAttribute("text.chunks.received", static_cast<int64_t>(session->getChunksReceived()));
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::streamingAdHocTextResponseToJson(response).dump());
}

PreparedResponse finishStreamingAdHoc(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed = parseBody(body, "ad-hoc.stream.finish", "streaming ad-hoc finish request",
                                  api::streamingAdHocFinishRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    if (span)
        span->setAttribute("session.id", canonicalUuid(request.sessionId));
    auto &manager = voice::StreamingAdHocSessionManager::instance();
    auto session = manager.getSession(request.sessionId);
    if (!session)
        return errorStatus(404, "Session not found", span);
    const auto completed = session->finish(span);
    if (!completed.isSuccess())
        return serverErrorStatus(completed.getError().value(), span);
    manager.removeSession(request.sessionId);
    const auto summary = completed.getValue().value();
    const api::StreamingAdHocFinishResponse response{request.sessionId,
                                                     "completed",
                                                     "Speech generated and playback triggered",
                                                     summary.lastAnimationId,
                                                     summary.partsRendered > 0,
                                                     summary.exchangeStatus,
                                                     summary.partsRendered,
                                                     summary.partsTotal};
    if (span) {
        span->setAttribute("exchange.status", summary.exchangeStatus);
        span->setAttribute("exchange.parts.rendered", static_cast<int64_t>(summary.partsRendered));
        span->setAttribute("exchange.parts.total", static_cast<int64_t>(summary.partsTotal));
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::streamingAdHocFinishResponseToJson(response).dump());
}

PreparedResponse listStreamingAdHocExchanges(const std::string &limitValue,
                                             const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::db)
        return errorStatus(500, "Ad-hoc exchange listing unavailable: database missing", span);
    int limit = api::DEFAULT_AD_HOC_EXCHANGE_LIMIT;
    if (!limitValue.empty()) {
        const auto parsed = api::adHocExchangeLimitFromString(limitValue);
        if (!parsed.isSuccess())
            return serverErrorStatus(parsed.getError().value(), span);
        limit = parsed.getValue().value();
    }
    if (span)
        span->setAttribute("query.limit", static_cast<int64_t>(limit));
    auto operation =
        creatures::observability
            ? creatures::observability->createChildOperationSpan("StreamingAdHocController.listExchanges", span)
            : nullptr;
    const auto result = creatures::db->listAdHocExchanges(limit, operation);
    if (!result.isSuccess())
        return serverErrorStatus(result.getError().value(), span);
    const auto records = result.getValue().value();
    auto values = nlohmann::json::array();
    for (const auto &record : records) {
        std::optional<std::string> finishedAt;
        if (record.exchange.finished_at_ms > 0) {
            finishedAt = formatTimeISO8601(
                std::chrono::system_clock::time_point(std::chrono::milliseconds(record.exchange.finished_at_ms)));
        }
        values.push_back(
            api::adHocExchangeResponseToJson(record.exchange, formatTimeISO8601(record.createdAt), finishedAt));
    }
    if (operation)
        operation->setSuccess();
    if (span) {
        span->setAttribute("exchanges.count", static_cast<int64_t>(records.size()));
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::listResponseToJson(values, [](const auto &value) { return value; }).dump());
}

PreparedResponse getStreamingAdHocExchange(const std::string &sessionId, const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::db)
        return errorStatus(500, "Ad-hoc exchange lookup unavailable: database missing", span);
    if (!isUuidShape(sessionId))
        return errorStatus(400, "sessionId must be a UUID", span);
    const auto canonicalSessionId = canonicalUuid(sessionId);
    if (span)
        span->setAttribute("session.id", canonicalSessionId);
    auto operation =
        creatures::observability
            ? creatures::observability->createChildOperationSpan("StreamingAdHocController.getExchange", span)
            : nullptr;
    const auto result = creatures::db->getAdHocExchange(canonicalSessionId, operation);
    if (!result.isSuccess())
        return serverErrorStatus(result.getError().value(), span);
    const auto record = result.getValue().value();
    std::optional<std::string> finishedAt;
    if (record.exchange.finished_at_ms > 0) {
        finishedAt = formatTimeISO8601(
            std::chrono::system_clock::time_point(std::chrono::milliseconds(record.exchange.finished_at_ms)));
    }
    if (operation)
        operation->setSuccess();
    if (span) {
        span->setAttribute("exchange.status", record.exchange.status);
        span->setSuccess();
    }
    return PreparedResponse::json(
        200, api::adHocExchangeResponseToJson(record.exchange, formatTimeISO8601(record.createdAt), finishedAt).dump());
}

} // namespace creatures::transport
