#pragma once

#include <oatpp-swagger/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "api/DialogStreamContracts.h"
#include "api/JsonResponse.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/voice/StreamingAdHocSession.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "util/JsonParser.h"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::ws {

/**
 * Streamed multi-character dialog (issue #186).
 *
 * The world composes a scene turn by turn and sends each turn here the moment
 * it exists; the server synthesizes it in that creature's voice, lip-syncs it
 * on that creature's channels, and plays it in arrival order while the other
 * participants hold a listening pose and look at the speaker. Same session
 * machinery as /ad-hoc-stream, which is the one-creature case; the exchange
 * read endpoints there serve these sessions too.
 */
class DialogStreamController : public oatpp::web::server::api::ApiController,
                               public HttpResponseHelpers<DialogStreamController> {
  public:
    DialogStreamController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    static std::shared_ptr<DialogStreamController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                                objectMapper)) {
        return std::make_shared<DialogStreamController>(objectMapper);
    }

    // --- Start a streaming dialog session ---

    ENDPOINT_INFO(startDialogStream) {
        info->summary = "Start a streaming dialog session for several creatures";
        info->description =
            "Creates a session with the given participants on a stage. Every creature listed is part of the "
            "exchange and must be registered on the same universe. Call /turn once per sentence, then /finish.";
        info->addTag("Streaming Dialog");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog-stream/start", startDialogStream,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog-stream/start", "POST", "api/v1/animation/dialog-stream/start",
            "startDialogStream", "DialogStreamController", request, [&](const auto &span) {
                if (!creatures::config || !creatures::db) {
                    return bailHttp(span, Status::CODE_500, "Streaming dialog unavailable: server dependencies missing",
                                    nullptr, "MissingDependencies");
                }
                const auto body = readRequestBodyLimited(request, api::MAX_STREAMING_AD_HOC_CONTROL_BODY_BYTES, span);
                const auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                                      "DialogStreamController.parseStartRequest", span)
                                                                : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "dialog_stream.start");
                const auto json = JsonParser::parseApiJsonString(body, "dialog stream start request", parseSpan);
                if (!json.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, json.getError().value());
                }
                const auto parsed = api::dialogStreamStartRequestFromJson(json.getValue().value());
                if (!parsed.isSuccess()) {
                    const auto error = parsed.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidDialogStreamStartRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                const auto requestValue = parsed.getValue().value();
                if (span) {
                    span->setAttribute("creature.id", canonicalUuid(requestValue.creatureIds.front()));
                    span->setAttribute("session.participants", static_cast<int64_t>(requestValue.creatureIds.size()));
                    span->setAttribute("stage.id", canonicalUuid(requestValue.stageId));
                    span->setAttribute("playback.resume.playlist", requestValue.resumePlaylist);
                }

                creatures::voice::StreamingSessionConfig sessionConfig;
                sessionConfig.creatureIds = requestValue.creatureIds;
                sessionConfig.stageId = requestValue.stageId;
                sessionConfig.resumePlaylist = requestValue.resumePlaylist;
                sessionConfig.dialog = true;

                auto &mgr = creatures::voice::StreamingAdHocSessionManager::instance();
                auto sessionResult = mgr.createSession(std::move(sessionConfig), span);
                if (!sessionResult.isSuccess())
                    return bailFromServerError(span, sessionResult.getError().value());
                auto session = sessionResult.getValue().value();

                auto startResult = session->start();
                if (!startResult.isSuccess()) {
                    mgr.removeSession(session->getSessionId());
                    if (span) {
                        span->setError(startResult.getError()->getMessage());
                    }
                    return bailFromServerError(span, startResult.getError().value());
                }

                const api::DialogStreamStartResponse response{
                    session->getSessionId(), "started", "Session started. Send turns via /turn, then call /finish.",
                    requestValue.creatureIds, requestValue.stageId};

                if (span) {
                    span->setAttribute("session.id", session->getSessionId());
                    span->setHttpStatus(200);
                }

                return jsonResponse(span, Status::CODE_200, api::dialogStreamStartResponseToJson(response));
            });
    }

    // --- Add a turn to a session ---

    ENDPOINT_INFO(addDialogStreamTurn) {
        info->summary = "Add one creature's turn to a streaming dialog";
        info->description = "One sentence (or short turn) spoken by one participant. Synthesized in that creature's "
                            "voice, queued and played in arrival order; turns for different creatures may interleave.";
        info->addTag("Streaming Dialog");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog-stream/turn", addDialogStreamTurn,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog-stream/turn", "POST", "api/v1/animation/dialog-stream/turn",
            "addDialogStreamTurn", "DialogStreamController", request, [&](const auto &span) {
                const auto body = readRequestBodyLimited(request, api::MAX_STREAMING_AD_HOC_TEXT_BODY_BYTES, span);
                const auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                                      "DialogStreamController.parseTurnRequest", span)
                                                                : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "dialog_stream.turn");
                const auto json = JsonParser::parseApiJsonString(body, "dialog stream turn request", parseSpan);
                if (!json.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, json.getError().value());
                }
                const auto parsed = api::dialogStreamTurnRequestFromJson(json.getValue().value());
                if (!parsed.isSuccess()) {
                    const auto error = parsed.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidDialogStreamTurnRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                const auto requestValue = parsed.getValue().value();
                if (span) {
                    span->setAttribute("session.id", canonicalUuid(requestValue.sessionId));
                    span->setAttribute("creature.id", canonicalUuid(requestValue.creatureId));
                    span->setAttribute("text.length", static_cast<int64_t>(requestValue.text.size()));
                }

                auto &mgr = creatures::voice::StreamingAdHocSessionManager::instance();
                auto session = mgr.getSession(requestValue.sessionId);
                if (!session) {
                    return bailHttp(span, Status::CODE_404, "Session not found");
                }

                auto addResult = session->addTurn(requestValue.creatureId, requestValue.text, span);
                if (!addResult.isSuccess()) {
                    if (span) {
                        span->setError(addResult.getError()->getMessage());
                    }
                    return bailFromServerError(span, addResult.getError().value());
                }

                const api::DialogStreamTurnResponse response{requestValue.sessionId, "ok",
                                                             session->getChunksReceived()};

                if (span) {
                    span->setAttribute("text.chunks.received", static_cast<int64_t>(session->getChunksReceived()));
                    span->setHttpStatus(200);
                }

                return jsonResponse(span, Status::CODE_200, api::dialogStreamTurnResponseToJson(response));
            });
    }

    // --- Finish a session ---

    ENDPOINT_INFO(finishDialogStream) {
        info->summary = "Finish a streaming dialog and stitch the exchange";
        info->description = "Waits for every queued turn to play, stitches the exchange into one WAV and one ad-hoc "
                            "animation carrying every participant's track, and records the exchange.";
        info->addTag("Streaming Dialog");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog-stream/finish", finishDialogStream,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog-stream/finish", "POST", "api/v1/animation/dialog-stream/finish",
            "finishDialogStream", "DialogStreamController", request, [&](const auto &span) {
                const auto body = readRequestBodyLimited(request, api::MAX_STREAMING_AD_HOC_CONTROL_BODY_BYTES, span);
                const auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                                      "DialogStreamController.parseFinishRequest", span)
                                                                : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "dialog_stream.finish");
                const auto json = JsonParser::parseApiJsonString(body, "dialog stream finish request", parseSpan);
                if (!json.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, json.getError().value());
                }
                const auto parsed = api::dialogStreamFinishRequestFromJson(json.getValue().value());
                if (!parsed.isSuccess()) {
                    const auto error = parsed.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidDialogStreamFinishRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                const auto requestValue = parsed.getValue().value();
                if (span)
                    span->setAttribute("session.id", canonicalUuid(requestValue.sessionId));

                auto &mgr = creatures::voice::StreamingAdHocSessionManager::instance();
                auto session = mgr.getSession(requestValue.sessionId);
                if (!session) {
                    return bailHttp(span, Status::CODE_404, "Session not found");
                }

                auto finishResult = session->finish(span);

                if (!finishResult.isSuccess()) {
                    if (span) {
                        span->setError(finishResult.getError()->getMessage());
                    }
                    return bailFromServerError(span, finishResult.getError().value());
                }

                // Only the request that reached terminal completion may free
                // the registry slot. A racing second /finish receives 409 and
                // must not make the still-running first request invisible.
                mgr.removeSession(requestValue.sessionId);

                const auto summary = finishResult.getValue().value();

                const api::DialogStreamFinishResponse response{requestValue.sessionId,
                                                               "completed",
                                                               "Dialog played and stitched",
                                                               summary.exchangeAnimationId,
                                                               summary.lastAnimationId,
                                                               summary.partsRendered > 0,
                                                               summary.exchangeStatus,
                                                               summary.partsRendered,
                                                               summary.partsTotal};

                if (span) {
                    if (!summary.exchangeAnimationId.empty())
                        span->setAttribute("animation.id", canonicalUuid(summary.exchangeAnimationId));
                    span->setAttribute("exchange.status", summary.exchangeStatus);
                    span->setAttribute("exchange.parts.rendered", static_cast<int64_t>(summary.partsRendered));
                    span->setAttribute("exchange.parts.total", static_cast<int64_t>(summary.partsTotal));
                    span->setHttpStatus(200);
                }

                return jsonResponse(span, Status::CODE_200, api::dialogStreamFinishResponseToJson(response));
            });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
