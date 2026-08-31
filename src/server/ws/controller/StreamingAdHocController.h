#pragma once

#include <filesystem>
#include <string_view>

#include <oatpp-swagger/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/outgoing/ResponseFactory.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "api/JsonResponse.h"
#include "api/StreamingAdHocContracts.h"
#include "model/AdHocExchange.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/voice/StreamingAdHocSession.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/dto/StatusDto.h"
#include "server/ws/service/SoundRenditionService.h"
#include "util/JsonParser.h"
#include "util/Slugify.h"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::ws {

class StreamingAdHocController : public oatpp::web::server::api::ApiController,
                                 public HttpResponseHelpers<StreamingAdHocController> {
  public:
    StreamingAdHocController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    static std::shared_ptr<StreamingAdHocController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                                  objectMapper)) {
        return std::make_shared<StreamingAdHocController>(objectMapper);
    }

    // --- Start a streaming session ---

    ENDPOINT_INFO(startStreamingAdHoc) {
        info->summary = "Start a streaming ad-hoc speech session";
        info->description = "Creates a session that accumulates text chunks from the agent. "
                            "Call /text to add sentences, then /finish to synthesize and play.";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc-stream/start", startStreamingAdHoc,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/ad-hoc-stream/start", "POST", "api/v1/animation/ad-hoc-stream/start",
            "startStreamingAdHoc", "StreamingAdHocController", request, [&](const auto &span) {
                if (!creatures::config || !creatures::db) {
                    return bailHttp(span, Status::CODE_500,
                                    "Streaming ad-hoc speech unavailable: server dependencies missing", nullptr,
                                    "MissingDependencies");
                }
                const auto body = readRequestBodyLimited(request, api::MAX_STREAMING_AD_HOC_CONTROL_BODY_BYTES, span);
                const auto parseSpan = creatures::observability
                                           ? creatures::observability->createChildOperationSpan(
                                                 "StreamingAdHocController.parseStartRequest", span)
                                           : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "streaming_ad_hoc.start");
                const auto json = JsonParser::parseApiJsonString(body, "streaming ad-hoc start request", parseSpan);
                if (!json.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, json.getError().value());
                }
                const auto parsed = api::streamingAdHocStartRequestFromJson(json.getValue().value());
                if (!parsed.isSuccess()) {
                    const auto error = parsed.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidStreamingStartRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                const auto requestValue = parsed.getValue().value();
                if (span) {
                    span->setAttribute("creature.id", canonicalUuid(requestValue.creatureId));
                    span->setAttribute("playback.resume_playlist", requestValue.resumePlaylist);
                }

                auto &mgr = creatures::voice::StreamingAdHocSessionManager::instance();
                auto sessionResult = mgr.createSession(requestValue.creatureId, requestValue.resumePlaylist, span);
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

                const api::StreamingAdHocStartResponse response{
                    session->getSessionId(), "started",
                    "Session started. Send text chunks via /text, then call /finish."};

                if (span) {
                    span->setAttribute("session.id", session->getSessionId());
                    span->setHttpStatus(200);
                }

                return jsonResponse(span, Status::CODE_200, api::streamingAdHocStartResponseToJson(response));
            });
    }

    // --- Add text to a session ---

    ENDPOINT_INFO(addStreamingAdHocText) {
        info->summary = "Add a text chunk to a streaming session";
        info->description = "Adds a sentence or text fragment to the session's speech buffer.";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc-stream/text", addStreamingAdHocText,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/ad-hoc-stream/text", "POST", "api/v1/animation/ad-hoc-stream/text",
            "addStreamingAdHocText", "StreamingAdHocController", request, [&](const auto &span) {
                const auto body = readRequestBodyLimited(request, api::MAX_STREAMING_AD_HOC_TEXT_BODY_BYTES, span);
                const auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                                      "StreamingAdHocController.parseTextRequest", span)
                                                                : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "streaming_ad_hoc.text");
                const auto json = JsonParser::parseApiJsonString(body, "streaming ad-hoc text request", parseSpan);
                if (!json.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, json.getError().value());
                }
                const auto parsed = api::streamingAdHocTextRequestFromJson(json.getValue().value());
                if (!parsed.isSuccess()) {
                    const auto error = parsed.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidStreamingTextRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                const auto requestValue = parsed.getValue().value();
                if (span) {
                    span->setAttribute("session.id", canonicalUuid(requestValue.sessionId));
                    span->setAttribute("text.length", static_cast<int64_t>(requestValue.text.size()));
                }

                auto &mgr = creatures::voice::StreamingAdHocSessionManager::instance();
                auto session = mgr.getSession(requestValue.sessionId);
                if (!session) {
                    return bailHttp(span, Status::CODE_404, "Session not found");
                }

                auto addResult = session->addText(requestValue.text, span);
                if (!addResult.isSuccess()) {
                    if (span) {
                        span->setError(addResult.getError()->getMessage());
                    }
                    return bailFromServerError(span, addResult.getError().value());
                }

                const api::StreamingAdHocTextResponse response{requestValue.sessionId, "ok",
                                                               session->getChunksReceived()};

                if (span) {
                    span->setAttribute("text.chunks_received", static_cast<int64_t>(session->getChunksReceived()));
                    span->setHttpStatus(200);
                }

                return jsonResponse(span, Status::CODE_200, api::streamingAdHocTextResponseToJson(response));
            });
    }

    // --- Finish a session ---

    ENDPOINT_INFO(finishStreamingAdHoc) {
        info->summary = "Finish a streaming session and trigger playback";
        info->description =
            "Sends all accumulated text to ElevenLabs, generates lip sync, builds animation, and plays it.";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc-stream/finish", finishStreamingAdHoc,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/ad-hoc-stream/finish", "POST", "api/v1/animation/ad-hoc-stream/finish",
            "finishStreamingAdHoc", "StreamingAdHocController", request, [&](const auto &span) {
                const auto body = readRequestBodyLimited(request, api::MAX_STREAMING_AD_HOC_CONTROL_BODY_BYTES, span);
                const auto parseSpan = creatures::observability
                                           ? creatures::observability->createChildOperationSpan(
                                                 "StreamingAdHocController.parseFinishRequest", span)
                                           : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "streaming_ad_hoc.finish");
                const auto json = JsonParser::parseApiJsonString(body, "streaming ad-hoc finish request", parseSpan);
                if (!json.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, json.getError().value());
                }
                const auto parsed = api::streamingAdHocFinishRequestFromJson(json.getValue().value());
                if (!parsed.isSuccess()) {
                    const auto error = parsed.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidStreamingFinishRequest", error.getCode());
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

                // Safe to remove now — finish() completes all TTS and animation
                // construction before returning. Playback is triggered via interrupt()
                // which creates its own PlaybackSession with its own lifecycle.
                mgr.removeSession(requestValue.sessionId);

                if (!finishResult.isSuccess()) {
                    if (span) {
                        span->setError(finishResult.getError()->getMessage());
                    }
                    return bailFromServerError(span, finishResult.getError().value());
                }

                const auto summary = finishResult.getValue().value();

                const api::StreamingAdHocFinishResponse response{requestValue.sessionId,
                                                                 "completed",
                                                                 "Speech generated and playback triggered",
                                                                 summary.lastAnimationId,
                                                                 summary.partsRendered > 0,
                                                                 summary.exchangeStatus,
                                                                 summary.partsRendered,
                                                                 summary.partsTotal};

                if (span) {
                    if (!summary.lastAnimationId.empty())
                        span->setAttribute("animation.id", canonicalUuid(summary.lastAnimationId));
                    span->setAttribute("exchange.status", summary.exchangeStatus);
                    span->setAttribute("exchange.parts_rendered", static_cast<int64_t>(summary.partsRendered));
                    span->setAttribute("exchange.parts_total", static_cast<int64_t>(summary.partsTotal));
                    span->setHttpStatus(200);
                }

                return jsonResponse(span, Status::CODE_200, api::streamingAdHocFinishResponseToJson(response));
            });
    }

    // --- Exchange export (issue #150) ---
    //
    // One "exchange" is one completed streaming session, stitched into a
    // single WAV with iXML provenance at /finish time. These endpoints list
    // exchanges and serve the stitched audio (raw, or rendered to MP3/Ogg
    // with full tags via the #148 machinery).

    ENDPOINT_INFO(listExchanges) {
        info->summary = "List streamed ad-hoc exchanges, newest first";
        info->description = "One exchange per streaming session, including in-flight ones (status 'streaming'). "
                            "TTL'd with the ad-hoc artifacts they reference.";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc-stream/exchanges", listExchanges,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/animation/ad-hoc-stream/exchanges", "GET", "api/v1/animation/ad-hoc-stream/exchanges",
            "listExchanges", "StreamingAdHocController", request, [&](const auto &span) {
                if (!creatures::db) {
                    return bailHttp(span, Status::CODE_500, "Ad-hoc exchange listing unavailable: database missing",
                                    nullptr, "MissingDependencies");
                }
                int limit = api::DEFAULT_AD_HOC_EXCHANGE_LIMIT;
                if (auto limitParam = request->getQueryParameter("limit")) {
                    auto limitResult = api::adHocExchangeLimitFromString(std::string(limitParam));
                    if (!limitResult.isSuccess())
                        return bailFromServerError(span, limitResult.getError().value());
                    limit = limitResult.getValue().value();
                }
                if (span)
                    span->setAttribute("query.limit", static_cast<int64_t>(limit));

                auto opSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                             "StreamingAdHocController.listExchanges", span)
                                                       : nullptr;
                auto listResult = creatures::db->listAdHocExchanges(limit, opSpan);
                if (!listResult.isSuccess()) {
                    if (opSpan)
                        opSpan->setError(listResult.getError()->getMessage());
                    return bailFromServerError(span, listResult.getError().value());
                }
                const auto records = listResult.getValue().value();
                if (opSpan) {
                    opSpan->setAttribute("exchanges.count", static_cast<int64_t>(records.size()));
                    opSpan->setSuccess();
                }

                const auto response =
                    api::listResponseToJson(records, [](const auto &record) { return exchangeResponseToJson(record); });

                if (span) {
                    span->setAttribute("exchanges.count", static_cast<int64_t>(records.size()));
                    span->setHttpStatus(200);
                }
                return jsonResponse(span, Status::CODE_200, response);
            });
    }

    ENDPOINT_INFO(getExchange) {
        info->summary = "Get one streamed ad-hoc exchange";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc-stream/exchange/{sessionId}", getExchange, PATH(String, sessionId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/animation/ad-hoc-stream/exchange/{sessionId}", "GET",
            "api/v1/animation/ad-hoc-stream/exchange/{sessionId}", "getExchange", "StreamingAdHocController", request,
            [&](const auto &span) {
                if (!creatures::db) {
                    return bailHttp(span, Status::CODE_500, "Ad-hoc exchange lookup unavailable: database missing",
                                    nullptr, "MissingDependencies");
                }
                if (!sessionId || !creatures::isUuidShape(std::string_view(sessionId->c_str(), sessionId->size()))) {
                    return bailHttp(span, Status::CODE_400, "sessionId must be a UUID");
                }
                const auto canonicalSessionId = creatures::canonicalUuid(
                    std::string_view(sessionId->c_str(), static_cast<std::size_t>(sessionId->size())));
                if (span)
                    span->setAttribute("session.id", canonicalSessionId);
                auto opSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                             "StreamingAdHocController.getExchange", span)
                                                       : nullptr;
                if (opSpan)
                    opSpan->setAttribute("session.id", canonicalSessionId);
                auto lookup = creatures::db->getAdHocExchange(canonicalSessionId, opSpan);
                if (!lookup.isSuccess()) {
                    if (opSpan)
                        opSpan->setError(lookup.getError()->getMessage());
                    return bailFromServerError(span, lookup.getError().value());
                }
                const auto record = lookup.getValue().value();
                if (opSpan) {
                    opSpan->setAttribute("exchange.status", record.exchange.status);
                    opSpan->setSuccess();
                }
                if (span) {
                    span->setAttribute("exchange.status", record.exchange.status);
                    span->setHttpStatus(200);
                }
                return jsonResponse(span, Status::CODE_200, exchangeResponseToJson(record));
            });
    }

    ENDPOINT_INFO(getExchangeAudioMp3) {
        info->summary = "Download a whole exchange as one fully-tagged MP3";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<String>(Status::CODE_200, "audio/mpeg");
        info->addResponse<Object<StatusDto>>(Status::CODE_409, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.mp3", getExchangeAudioMp3,
             PATH(String, sessionId), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.mp3", "GET",
                           "api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.mp3", "getExchangeAudioMp3",
                           "StreamingAdHocController", request,
                           [&](const auto &span) { return serveExchangeAudio(sessionId, ExchangeAudio::Mp3, span); });
    }

    ENDPOINT_INFO(getExchangeAudioOgg) {
        info->summary = "Download a whole exchange as one tagged Ogg/Opus file";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<String>(Status::CODE_200, "audio/ogg");
        info->addResponse<Object<StatusDto>>(Status::CODE_409, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.ogg", getExchangeAudioOgg,
             PATH(String, sessionId), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.ogg", "GET",
                           "api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.ogg", "getExchangeAudioOgg",
                           "StreamingAdHocController", request,
                           [&](const auto &span) { return serveExchangeAudio(sessionId, ExchangeAudio::Ogg, span); });
    }

    ENDPOINT_INFO(getExchangeAudioWav) {
        info->summary = "Download a whole exchange as the stitched 17-channel WAV";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<String>(Status::CODE_200, "audio/wav");
        info->addResponse<Object<StatusDto>>(Status::CODE_409, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.wav", getExchangeAudioWav,
             PATH(String, sessionId), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.wav", "GET",
                           "api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.wav", "getExchangeAudioWav",
                           "StreamingAdHocController", request,
                           [&](const auto &span) { return serveExchangeAudio(sessionId, ExchangeAudio::Wav, span); });
    }

  private:
    enum class ExchangeAudio { Wav, Mp3, Ogg };

    static nlohmann::json exchangeResponseToJson(const creatures::AdHocExchangeRecord &record) {
        std::optional<std::string> finishedAt;
        if (record.exchange.finished_at_ms > 0) {
            finishedAt = formatTimeISO8601(
                std::chrono::system_clock::time_point(std::chrono::milliseconds(record.exchange.finished_at_ms)));
        }
        return api::adHocExchangeResponseToJson(record.exchange, formatTimeISO8601(record.createdAt), finishedAt);
    }

    /// Download filename in the shared export shape (#126, #152): slugified
    /// title plus a short session-id tail, so identically-worded exchanges
    /// don't collide. e.g. "beaky-somebody-is-at-the-door-e3af1c4d.mp3"
    static std::string attachmentBasename(const creatures::AdHocExchange &exchange) {
        return util::exportBasename(exchange.title.empty() ? exchange.creature_name : exchange.title,
                                    exchange.session_id, 48, "exchange");
    }

    template <typename SpanT>
    std::shared_ptr<HttpOutgoingResponse> serveExchangeAudio(const oatpp::String &sessionId, ExchangeAudio format,
                                                             const SpanT &span) {
        if (!creatures::db) {
            return bailHttp(span, Status::CODE_500, "Ad-hoc exchange audio unavailable: database missing", nullptr,
                            "MissingDependencies");
        }
        if (!sessionId || !creatures::isUuidShape(std::string_view(sessionId->c_str(), sessionId->size()))) {
            return bailHttp(span, Status::CODE_400, "sessionId must be a UUID");
        }
        const auto canonicalSessionId =
            creatures::canonicalUuid(std::string_view(sessionId->c_str(), static_cast<std::size_t>(sessionId->size())));
        if (span)
            span->setAttribute("session.id", canonicalSessionId);
        auto opSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                     "StreamingAdHocController.serveExchangeAudio", span)
                                               : nullptr;
        if (opSpan)
            opSpan->setAttribute("session.id", canonicalSessionId);
        auto lookup = creatures::db->getAdHocExchange(canonicalSessionId, opSpan);
        if (!lookup.isSuccess()) {
            if (opSpan)
                opSpan->setError(lookup.getError()->getMessage());
            return bailFromServerError(span, lookup.getError().value());
        }
        const auto record = lookup.getValue().value();
        const auto &exchange = record.exchange;
        if (opSpan) {
            opSpan->setAttribute("exchange.status", exchange.status);
            opSpan->setSuccess();
        }
        if (span) {
            span->setAttribute("session.id", exchange.session_id);
            span->setAttribute("exchange.status", exchange.status);
        }

        if (exchange.status == EXCHANGE_STATUS_STREAMING) {
            auto response = bailHttp(span, Status::CODE_409, "Exchange is still streaming; try again shortly");
            response->putHeader("Retry-After", "5");
            return response;
        }
        if (exchange.status == EXCHANGE_STATUS_FAILED || exchange.sound_file.empty()) {
            return bailHttp(span, Status::CODE_410, "Exchange rendered no audio");
        }
        const std::filesystem::path wavPath(exchange.sound_file);
        if (!std::filesystem::exists(wavPath)) {
            return bailHttp(span, Status::CODE_404, "Exchange audio has expired");
        }

        std::shared_ptr<HttpOutgoingResponse> response;
        std::string mimeType;
        std::string extension;
        int64_t bodyBytes = 0;
        if (format == ExchangeAudio::Wav) {
            // Streamed, not buffered (#140): a long exchange's 17-channel WAV
            // runs to hundreds of MB and must never be slurped into memory.
            std::error_code sizeError;
            const auto fileSize = std::filesystem::file_size(wavPath, sizeError);
            if (sizeError) {
                return bailHttp(span, Status::CODE_404, "Exchange audio has expired");
            }
            auto body = std::make_shared<FileBody>(wavPath.string(), static_cast<v_int64>(fileSize));
            if (!body->isOpen()) {
                return bailHttp(span, Status::CODE_500, "Unable to read exchange audio");
            }
            response = OutgoingResponse::createShared(Status::CODE_200, body);
            mimeType = "audio/wav";
            extension = ".wav";
            bodyBytes = static_cast<int64_t>(fileSize);
        } else {
            const auto renditionFormat =
                format == ExchangeAudio::Mp3 ? SoundRenditionFormat::Mp3 : SoundRenditionFormat::OggOpus;
            auto encoded = renditionService_.renderWav(wavPath, renditionFormat);
            if (!encoded.isSuccess()) {
                return bailFromServerError(span, encoded.getError().value());
            }
            const auto rendition = encoded.getValue().value();
            response = ResponseFactory::createResponse(
                Status::CODE_200, oatpp::String(reinterpret_cast<const char *>(rendition.bytes.data()),
                                                static_cast<v_buff_size>(rendition.bytes.size())));
            mimeType = rendition.mimeType;
            extension = rendition.extension;
            bodyBytes = static_cast<int64_t>(rendition.bytes.size());
        }

        const auto attachmentName = attachmentBasename(exchange) + extension;
        response->putHeader("Content-Type", mimeType.c_str());
        response->putHeader("Content-Disposition", "attachment; filename=\"" + attachmentName + "\"");
        // A UUID-addressed exchange can never change once finalized, and the
        // encoders are deterministic — immutability is honest here, unlike the
        // basename-addressed ad-hoc renditions (which stay no-store).
        response->putHeader("Cache-Control", "public, max-age=31536000, immutable");
        if (span) {
            span->setAttribute("rendition.bytes", bodyBytes);
            span->setHttpStatus(200);
        }
        return response;
    }

    creatures::ws::SoundRenditionService renditionService_;
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
