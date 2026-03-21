#pragma once

#include <oatpp-swagger/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "server/namespace-stuffs.h"
#include "server/voice/StreamingAdHocSession.h"
#include "server/ws/dto/StatusDto.h"
#include "server/ws/dto/StreamingAdHocDto.h"
#include "util/ObservabilityManager.h"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::ws {

class StreamingAdHocController : public oatpp::web::server::api::ApiController {
  public:
    StreamingAdHocController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    static std::shared_ptr<StreamingAdHocController> createShared(
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) {
        return std::make_shared<StreamingAdHocController>(objectMapper);
    }

    // --- Start a streaming session ---

    ENDPOINT_INFO(startStreamingAdHoc) {
        info->summary = "Start a streaming ad-hoc speech session";
        info->description = "Creates a session that accumulates text chunks from the agent. "
                            "Call /text to add sentences, then /finish to synthesize and play.";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<Object<StreamingAdHocStartResponseDto>>(Status::CODE_200,
                                                                    "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc-stream/start", startStreamingAdHoc,
             BODY_DTO(Object<StreamingAdHocStartRequestDto>, requestBody),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {

        auto span = creatures::observability
                        ? creatures::observability->createRequestSpan(
                              "POST /api/v1/animation/ad-hoc-stream/start", "POST",
                              "api/v1/animation/ad-hoc-stream/start", extractTraceparent(request))
                        : nullptr;
        addHttpRequestAttributes(span, request);

        auto creatureId = requestBody->creature_id;
        bool resumePlaylist = requestBody->resume_playlist != nullptr ? *requestBody->resume_playlist : true;

        if (!creatureId || creatureId->empty()) {
            auto result = StatusDto::createShared();
            result->status = "error";
            result->code = 400;
            result->message = "creature_id is required";
            return createDtoResponse(Status::CODE_400, result);
        }

        auto &mgr = creatures::voice::StreamingAdHocSessionManager::instance();
        auto session = mgr.createSession(creatureId->c_str(), resumePlaylist, span);

        auto startResult = session->start();
        if (!startResult.isSuccess()) {
            mgr.removeSession(session->getSessionId());
            auto result = StatusDto::createShared();
            result->status = "error";
            result->code = 500;
            result->message = startResult.getError()->getMessage().c_str();
            return createDtoResponse(Status::CODE_500, result);
        }

        auto response = StreamingAdHocStartResponseDto::createShared();
        response->session_id = session->getSessionId().c_str();
        response->status = "started";
        response->message = "Session started. Send text chunks via /text, then call /finish.";

        if (span) {
            span->setAttribute("session.id", session->getSessionId());
            span->setHttpStatus(200);
        }

        return createDtoResponse(Status::CODE_200, response);
    }

    // --- Add text to a session ---

    ENDPOINT_INFO(addStreamingAdHocText) {
        info->summary = "Add a text chunk to a streaming session";
        info->description = "Adds a sentence or text fragment to the session's speech buffer.";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<Object<StreamingAdHocTextResponseDto>>(Status::CODE_200,
                                                                   "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc-stream/text", addStreamingAdHocText,
             BODY_DTO(Object<StreamingAdHocTextRequestDto>, requestBody),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {

        auto span = creatures::observability
                        ? creatures::observability->createRequestSpan(
                              "POST /api/v1/animation/ad-hoc-stream/text", "POST",
                              "api/v1/animation/ad-hoc-stream/text", extractTraceparent(request))
                        : nullptr;
        addHttpRequestAttributes(span, request);

        auto sessionId = requestBody->session_id;
        auto text = requestBody->text;

        if (!sessionId || sessionId->empty() || !text || text->empty()) {
            auto result = StatusDto::createShared();
            result->status = "error";
            result->code = 400;
            result->message = "session_id and text are required";
            return createDtoResponse(Status::CODE_400, result);
        }

        auto &mgr = creatures::voice::StreamingAdHocSessionManager::instance();
        auto session = mgr.getSession(sessionId->c_str());
        if (!session) {
            auto result = StatusDto::createShared();
            result->status = "error";
            result->code = 404;
            result->message = "Session not found";
            return createDtoResponse(Status::CODE_404, result);
        }

        auto addResult = session->addText(text->c_str());
        if (!addResult.isSuccess()) {
            auto result = StatusDto::createShared();
            result->status = "error";
            result->code = 500;
            result->message = addResult.getError()->getMessage().c_str();
            return createDtoResponse(Status::CODE_500, result);
        }

        auto response = StreamingAdHocTextResponseDto::createShared();
        response->session_id = sessionId;
        response->status = "ok";
        response->chunks_received = session->getChunksReceived();

        if (span) {
            span->setAttribute("session.id", std::string(sessionId->c_str()));
            span->setAttribute("text.length", static_cast<int64_t>(text->size()));
            span->setHttpStatus(200);
        }

        return createDtoResponse(Status::CODE_200, response);
    }

    // --- Finish a session ---

    ENDPOINT_INFO(finishStreamingAdHoc) {
        info->summary = "Finish a streaming session and trigger playback";
        info->description =
            "Sends all accumulated text to ElevenLabs, generates lip sync, builds animation, and plays it.";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<Object<StreamingAdHocFinishResponseDto>>(Status::CODE_200,
                                                                     "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc-stream/finish", finishStreamingAdHoc,
             BODY_DTO(Object<StreamingAdHocFinishRequestDto>, requestBody),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {

        auto span = creatures::observability
                        ? creatures::observability->createRequestSpan(
                              "POST /api/v1/animation/ad-hoc-stream/finish", "POST",
                              "api/v1/animation/ad-hoc-stream/finish", extractTraceparent(request))
                        : nullptr;
        addHttpRequestAttributes(span, request);

        auto sessionId = requestBody->session_id;

        if (!sessionId || sessionId->empty()) {
            auto result = StatusDto::createShared();
            result->status = "error";
            result->code = 400;
            result->message = "session_id is required";
            return createDtoResponse(Status::CODE_400, result);
        }

        auto &mgr = creatures::voice::StreamingAdHocSessionManager::instance();
        auto session = mgr.getSession(sessionId->c_str());
        if (!session) {
            auto result = StatusDto::createShared();
            result->status = "error";
            result->code = 404;
            result->message = "Session not found";
            return createDtoResponse(Status::CODE_404, result);
        }

        auto finishResult = session->finish();

        // Always clean up the session
        mgr.removeSession(sessionId->c_str());

        if (!finishResult.isSuccess()) {
            auto result = StatusDto::createShared();
            result->status = "error";
            result->code = 500;
            result->message = finishResult.getError()->getMessage().c_str();
            if (span) {
                span->setError(finishResult.getError()->getMessage());
                span->setHttpStatus(500);
            }
            return createDtoResponse(Status::CODE_500, result);
        }

        auto animationId = finishResult.getValue().value();

        auto response = StreamingAdHocFinishResponseDto::createShared();
        response->session_id = sessionId;
        response->status = "completed";
        response->message = "Speech generated and playback triggered";
        response->animation_id = animationId.c_str();
        response->playback_triggered = true;

        if (span) {
            span->setAttribute("session.id", std::string(sessionId->c_str()));
            span->setAttribute("animation.id", animationId);
            span->setHttpStatus(200);
        }

        return createDtoResponse(Status::CODE_200, response);
    }

  private:
    static std::string extractTraceparent(
        const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request) {
        auto header = request->getHeader("traceparent");
        return header ? std::string(header->c_str()) : "";
    }

    static void addHttpRequestAttributes(std::shared_ptr<creatures::RequestSpan> span,
                                          const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request) {
        if (!span) return;
        span->setAttribute("http.method", std::string(request->getStartingLine().method.std_str()));
        span->setAttribute("http.url", std::string(request->getStartingLine().path.std_str()));
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
