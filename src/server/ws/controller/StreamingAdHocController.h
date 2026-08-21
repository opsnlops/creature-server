#pragma once

#include <filesystem>
#include <string_view>

#include <oatpp-swagger/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/outgoing/ResponseFactory.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "model/AdHocExchange.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/voice/StreamingAdHocSession.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/dto/StatusDto.h"
#include "server/ws/dto/StreamingAdHocDto.h"
#include "server/ws/service/SoundRenditionService.h"
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
        info->addResponse<Object<StreamingAdHocStartResponseDto>>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc-stream/start", startStreamingAdHoc,
             BODY_DTO(Object<StreamingAdHocStartRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/animation/ad-hoc-stream/start", "POST", "api/v1/animation/ad-hoc-stream/start",
                           "startStreamingAdHoc", "StreamingAdHocController", request, [&](const auto &span) {
                               auto creatureId = requestBody->creature_id;
                               bool resumePlaylist =
                                   requestBody->resume_playlist != nullptr ? *requestBody->resume_playlist : true;

                               if (!creatureId || creatureId->empty()) {
                                   return bailHttp(span, Status::CODE_400, "creature_id is required");
                               }

                               auto &mgr = creatures::voice::StreamingAdHocSessionManager::instance();
                               auto session = mgr.createSession(creatureId->c_str(), resumePlaylist, span);

                               auto startResult = session->start();
                               if (!startResult.isSuccess()) {
                                   mgr.removeSession(session->getSessionId());
                                   if (span) {
                                       span->setError(startResult.getError()->getMessage());
                                   }
                                   return bailFromServerError(span, startResult.getError().value());
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
                           });
    }

    // --- Add text to a session ---

    ENDPOINT_INFO(addStreamingAdHocText) {
        info->summary = "Add a text chunk to a streaming session";
        info->description = "Adds a sentence or text fragment to the session's speech buffer.";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<Object<StreamingAdHocTextResponseDto>>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc-stream/text", addStreamingAdHocText,
             BODY_DTO(Object<StreamingAdHocTextRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/animation/ad-hoc-stream/text", "POST", "api/v1/animation/ad-hoc-stream/text",
                           "addStreamingAdHocText", "StreamingAdHocController", request, [&](const auto &span) {
                               auto sessionId = requestBody->session_id;
                               auto text = requestBody->text;

                               if (!sessionId || sessionId->empty() || !text || text->empty()) {
                                   return bailHttp(span, Status::CODE_400, "session_id and text are required");
                               }

                               auto &mgr = creatures::voice::StreamingAdHocSessionManager::instance();
                               auto session = mgr.getSession(sessionId->c_str());
                               if (!session) {
                                   return bailHttp(span, Status::CODE_404, "Session not found");
                               }

                               auto addResult = session->addText(text->c_str());
                               if (!addResult.isSuccess()) {
                                   if (span) {
                                       span->setError(addResult.getError()->getMessage());
                                   }
                                   return bailFromServerError(span, addResult.getError().value());
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
                           });
    }

    // --- Finish a session ---

    ENDPOINT_INFO(finishStreamingAdHoc) {
        info->summary = "Finish a streaming session and trigger playback";
        info->description =
            "Sends all accumulated text to ElevenLabs, generates lip sync, builds animation, and plays it.";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<Object<StreamingAdHocFinishResponseDto>>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc-stream/finish", finishStreamingAdHoc,
             BODY_DTO(Object<StreamingAdHocFinishRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/animation/ad-hoc-stream/finish", "POST",
                           "api/v1/animation/ad-hoc-stream/finish", "finishStreamingAdHoc", "StreamingAdHocController",
                           request, [&](const auto &span) {
                               auto sessionId = requestBody->session_id;

                               if (!sessionId || sessionId->empty()) {
                                   return bailHttp(span, Status::CODE_400, "session_id is required");
                               }

                               auto &mgr = creatures::voice::StreamingAdHocSessionManager::instance();
                               auto session = mgr.getSession(sessionId->c_str());
                               if (!session) {
                                   return bailHttp(span, Status::CODE_404, "Session not found");
                               }

                               auto finishResult = session->finish();

                               // Safe to remove now — finish() completes all TTS and animation
                               // construction before returning. Playback is triggered via interrupt()
                               // which creates its own PlaybackSession with its own lifecycle.
                               mgr.removeSession(sessionId->c_str());

                               if (!finishResult.isSuccess()) {
                                   if (span) {
                                       span->setError(finishResult.getError()->getMessage());
                                   }
                                   return bailFromServerError(span, finishResult.getError().value());
                               }

                               const auto summary = finishResult.getValue().value();

                               auto response = StreamingAdHocFinishResponseDto::createShared();
                               response->session_id = sessionId;
                               response->status = "completed";
                               response->message = "Speech generated and playback triggered";
                               response->animation_id = summary.lastAnimationId.c_str();
                               response->playback_triggered = summary.partsRendered > 0;
                               response->exchange_status = summary.exchangeStatus.c_str();
                               response->parts_rendered = summary.partsRendered;
                               response->parts_total = summary.partsTotal;

                               if (span) {
                                   span->setAttribute("session.id", std::string(sessionId->c_str()));
                                   span->setAttribute("animation.id", summary.lastAnimationId);
                                   span->setAttribute("exchange.status", summary.exchangeStatus);
                                   span->setHttpStatus(200);
                               }

                               return createDtoResponse(Status::CODE_200, response);
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
        info->addResponse<Object<AdHocExchangeListDto>>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc-stream/exchanges", listExchanges,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/animation/ad-hoc-stream/exchanges", "GET",
                           "api/v1/animation/ad-hoc-stream/exchanges", "listExchanges", "StreamingAdHocController",
                           request, [&](const auto &span) {
                               int limit = 50;
                               if (auto limitParam = request->getQueryParameter("limit")) {
                                   try {
                                       limit = std::stoi(std::string(limitParam->c_str()));
                                   } catch (const std::exception &) {
                                       return bailHttp(span, Status::CODE_400, "limit must be an integer");
                                   }
                                   if (limit < 1 || limit > 500) {
                                       return bailHttp(span, Status::CODE_400, "limit must be between 1 and 500");
                                   }
                               }

                               auto opSpan = creatures::observability
                                                 ? creatures::observability->createChildOperationSpan(
                                                       "StreamingAdHocController.listExchanges", span)
                                                 : nullptr;
                               auto listResult = creatures::db->listAdHocExchanges(limit, opSpan);
                               if (!listResult.isSuccess()) {
                                   return bailFromServerError(span, listResult.getError().value());
                               }
                               const auto records = listResult.getValue().value();

                               auto response = AdHocExchangeListDto::createShared();
                               response->items = oatpp::Vector<oatpp::Object<AdHocExchangeDto>>::createShared();
                               for (const auto &record : records) {
                                   response->items->push_back(convertToDto(record.exchange, record.createdAt));
                               }
                               response->count = static_cast<uint32_t>(response->items->size());

                               if (span) {
                                   span->setAttribute("exchanges.count", static_cast<int64_t>(records.size()));
                                   span->setHttpStatus(200);
                               }
                               return createDtoResponse(Status::CODE_200, response);
                           });
    }

    ENDPOINT_INFO(getExchange) {
        info->summary = "Get one streamed ad-hoc exchange";
        info->addTag("Streaming Ad-Hoc Speech");
        info->addResponse<Object<AdHocExchangeDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc-stream/exchange/{sessionId}", getExchange, PATH(String, sessionId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/animation/ad-hoc-stream/exchange/{sessionId}", "GET",
            "api/v1/animation/ad-hoc-stream/exchange/{sessionId}", "getExchange", "StreamingAdHocController", request,
            [&](const auto &span) {
                if (!isUuid(sessionId)) {
                    return bailHttp(span, Status::CODE_400, "sessionId must be a UUID");
                }
                auto opSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                             "StreamingAdHocController.getExchange", span)
                                                       : nullptr;
                auto lookup = creatures::db->getAdHocExchange(sessionId->c_str(), opSpan);
                if (!lookup.isSuccess()) {
                    return bailFromServerError(span, lookup.getError().value());
                }
                const auto record = lookup.getValue().value();
                if (span) {
                    span->setAttribute("session.id", std::string(sessionId->c_str()));
                    span->setAttribute("exchange.status", record.exchange.status);
                    span->setHttpStatus(200);
                }
                return createDtoResponse(Status::CODE_200, convertToDto(record.exchange, record.createdAt));
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

    /// Strict UUID shape check — the session id becomes part of an on-disk
    /// path, so nothing but the canonical 8-4-4-4-12 hex layout gets anywhere
    /// near the filesystem. Delegates to the codebase's one trust-boundary
    /// UUID gate (security review M4).
    static bool isUuid(const oatpp::String &value) {
        if (!value) {
            return false;
        }
        return creatures::isUuidShape(std::string_view(value->c_str(), value->size()));
    }

    /// Download filename in the #126 dialog-export shape: slugified title plus
    /// a short session-id tail, so identically-worded exchanges don't collide.
    /// e.g. "beaky-somebody-is-at-the-door-e3af1c4d.mp3"
    static std::string attachmentBasename(const creatures::AdHocExchange &exchange) {
        const auto titleSlug =
            util::slugify(exchange.title.empty() ? exchange.creature_name : exchange.title, 48, "exchange");
        return fmt::format("{}-{}", titleSlug, exchange.session_id.substr(0, 8));
    }

    template <typename SpanT>
    std::shared_ptr<HttpOutgoingResponse> serveExchangeAudio(const oatpp::String &sessionId, ExchangeAudio format,
                                                             const SpanT &span) {
        if (!isUuid(sessionId)) {
            return bailHttp(span, Status::CODE_400, "sessionId must be a UUID");
        }
        auto opSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                     "StreamingAdHocController.serveExchangeAudio", span)
                                               : nullptr;
        auto lookup = creatures::db->getAdHocExchange(sessionId->c_str(), opSpan);
        if (!lookup.isSuccess()) {
            return bailFromServerError(span, lookup.getError().value());
        }
        const auto record = lookup.getValue().value();
        const auto &exchange = record.exchange;
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
