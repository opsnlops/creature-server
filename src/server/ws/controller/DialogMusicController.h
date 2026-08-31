#pragma once

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>

#include <fmt/format.h>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/outgoing/ResponseFactory.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "api/DialogContracts.h"
#include "api/JobResponses.h"
#include "model/DialogScript.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/voice/MusicClient.h"
#include "server/voice/MusicGenerationCache.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/service/DialogMusicService.h"
#include "server/ws/service/SoundRenditionService.h"
#include "util/JsonParser.h"
#include "util/Sha256.h"
#include "util/Slugify.h"
#include "util/helpers.h"

namespace creatures {
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures::ws {

class DialogMusicController : public oatpp::web::server::api::ApiController,
                              public HttpResponseHelpers<DialogMusicController> {
  public:
    DialogMusicController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) : ApiController(objectMapper) {}

    static std::shared_ptr<DialogMusicController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                               objectMapper)) {
        return std::make_shared<DialogMusicController>(objectMapper);
    }

  private:
    DialogMusicService musicService_;
    SoundRenditionService renditionService_;
    std::mutex renditionMutex_;

  public:
    ENDPOINT_INFO(submitDialogMusic) {
        info->summary = "Generate instrumental background music for an exact cached dialog take";
        info->description =
            "Returns a job id. Progress/completion arrive through the existing job WebSocket; the "
            "completion result contains an immutable MP3 preview URL. The 48 kHz WAV stays server-side.";
        info->addTag("Multi-character Dialog");
        info->addResponse<oatpp::String>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_429, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/music", submitDialogMusic,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/music", "POST", "api/v1/animation/dialog/music", "submitDialogMusic",
            "DialogMusicController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                const auto body = readRequestBodyLimited(request, api::MAX_DIALOG_REQUEST_BYTES, span);
                const auto parseSpan = creatures::observability
                                           ? creatures::observability->createChildOperationSpan(
                                                 "DialogMusicController.parseDialogMusicRequest", span)
                                           : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "dialog.music.submit");
                const auto json = JsonParser::parseApiJsonString(body, "dialog music request", parseSpan);
                if (!json.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, json.getError().value());
                }
                const auto parsed = api::dialogMusicRequestFromJson(json.getValue().value());
                if (!parsed.isSuccess()) {
                    const auto error = parsed.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidDialogMusicRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                const auto musicRequest = parsed.getValue().value();
                if (span) {
                    span->setAttribute("dialog.script_id", canonicalUuid(musicRequest.scriptId));
                    span->setAttribute("dialog.generation_id", canonicalUuid(musicRequest.dialogGenerationId));
                    span->setAttribute("dialog.cache_key", musicRequest.dialogCacheKey);
                    span->setAttribute("music.generation_mode", musicRequest.generationMode);
                    span->setAttribute("music.prompt_length", static_cast<int64_t>(musicRequest.prompt.size()));
                    span->setAttribute("music.duration_extension_ms", musicRequest.durationExtensionMs);
                }
                const auto details = api::dialogMusicRequestToJson(musicRequest).dump();
                const auto admission = creatures::jobWorker->tryCreateAndQueueMusicJob(details, span);
                if (admission.status == creatures::jobs::JobWorker::QueueAdmission::Status::Full) {
                    return bailHttp(span, Status::CODE_429,
                                    "Two music generations are already queued or running; try again shortly", nullptr,
                                    "QueueAdmissionRejected");
                }
                if (admission.status == creatures::jobs::JobWorker::QueueAdmission::Status::EnqueueFailed) {
                    return bailHttp(span, Status::CODE_500, "Could not queue dialog music job", nullptr,
                                    "QueueEnqueueFailure");
                }
                const auto &jobId = admission.jobId;
                if (span) {
                    span->setAttribute("job.id", jobId);
                    span->setHttpStatus(202);
                }
                const api::JobCreatedResponse response{
                    jobId, "dialog-music",
                    "Dialog music job created; listen for job-progress and job-complete messages."};
                return jsonResponse(span, Status::CODE_202, api::jobCreatedResponseToJson(response));
            });
    }

    ENDPOINT_INFO(getGeneratedMusicMp3) {
        info->summary = "Get an immutable MP3 audition rendition of a generated music take";
        info->addTag("Multi-character Dialog");
        info->addResponse<String>(Status::CODE_200, "audio/mpeg");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/dialog/music/generated/{filename}", getGeneratedMusicMp3, PATH(String, filename),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/animation/dialog/music/generated/{filename}", "GET",
            "api/v1/animation/dialog/music/generated/{filename}", "getGeneratedMusicMp3", "DialogMusicController",
            request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                std::string value = filename ? std::string(*filename) : std::string{};
                if (!value.ends_with(".mp3"))
                    return bailHttp(span, Status::CODE_422, "generated music URLs must end in .mp3");
                value.resize(value.size() - 4);
                if (!isUuidShape(value))
                    return bailHttp(span, Status::CODE_400, "music generation id must be a UUID");
                if (span)
                    span->setAttribute("music.generation_id", value);
                auto generation = voice::loadMusicGeneration(value);
                if (!generation.isSuccess())
                    return bailFromServerError(span, generation.getError().value());
                auto renditionSpan =
                    creatures::observability->createChildOperationSpan("SoundRenditionService.renderWav", span);
                if (renditionSpan) {
                    renditionSpan->setAttribute("music.generation_id", value);
                    renditionSpan->setAttribute("rendition.format", "mp3");
                    renditionSpan->setAttribute("sound.source", "music_generation_cache");
                    renditionSpan->setAttribute("sound.file_hash",
                                                util::sha256Hex(generation.getValue().value().wavPath.string()));
                }
                SoundRendition rendered;
                {
                    // Immutable candidate URLs share one atomic on-disk MP3.
                    // Serializing cache misses prevents concurrent refreshes
                    // from multiplying encoder CPU and memory.
                    const std::scoped_lock renditionLock(renditionMutex_);
                    auto cached = voice::loadMusicMp3(value);
                    if (!cached.isSuccess()) {
                        recordSpanError(renditionSpan, cached.getError().value().getMessage(), "RenditionCacheError",
                                        cached.getError().value().getCode());
                        return bailFromServerError(span, cached.getError().value());
                    }
                    auto cachedBytes = cached.getValue().value();
                    if (cachedBytes) {
                        rendered.bytes = std::move(*cachedBytes);
                        rendered.mimeType = "audio/mpeg";
                        rendered.extension = ".mp3";
                        if (renditionSpan) {
                            renditionSpan->setAttribute("cache.hit", true);
                            renditionSpan->setAttribute("cache.outcome", "hit");
                            renditionSpan->setAttribute("encoding.performed", false);
                        }
                    } else {
                        if (renditionSpan) {
                            std::error_code fileSizeError;
                            const auto inputBytes =
                                std::filesystem::file_size(generation.getValue().value().wavPath, fileSizeError);
                            if (!fileSizeError)
                                renditionSpan->setAttribute("encoding.input_bytes", static_cast<int64_t>(inputBytes));
                        }
                        auto rendition = renditionService_.renderWav(generation.getValue().value().wavPath,
                                                                     SoundRenditionFormat::Mp3);
                        if (!rendition.isSuccess()) {
                            recordSpanError(renditionSpan, rendition.getError().value().getMessage(), "RenditionError",
                                            rendition.getError().value().getCode());
                            return bailFromServerError(span, rendition.getError().value());
                        }
                        rendered = rendition.getValue().value();
                        auto savedMp3 = voice::saveMusicMp3(value, rendered.bytes);
                        if (!savedMp3.isSuccess()) {
                            recordSpanError(renditionSpan, savedMp3.getError().value().getMessage(),
                                            "RenditionCacheError", savedMp3.getError().value().getCode());
                            return bailFromServerError(span, savedMp3.getError().value());
                        }
                        if (renditionSpan) {
                            renditionSpan->setAttribute("cache.hit", false);
                            renditionSpan->setAttribute("cache.outcome", "miss");
                            renditionSpan->setAttribute("encoding.performed", true);
                        }
                    }
                }
                if (renditionSpan) {
                    renditionSpan->setAttribute("rendition.output_bytes", static_cast<int64_t>(rendered.bytes.size()));
                    renditionSpan->setSuccess();
                }
                const auto downloadName = util::bgmExportBasename(generation.getValue().value().title,
                                                                  generation.getValue().value().prompt, value) +
                                          ".mp3";
                auto response = ResponseFactory::createResponse(
                    Status::CODE_200, oatpp::String(reinterpret_cast<const char *>(rendered.bytes.data()),
                                                    static_cast<v_buff_size>(rendered.bytes.size())));
                response->putHeader("Content-Type", rendered.mimeType.c_str());
                response->putHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
                response->putHeader("Cache-Control", "public, max-age=31536000, immutable");
                response->putHeader("X-Music-Generation-Id", value.c_str());
                if (span) {
                    span->setAttribute("rendition.bytes", static_cast<int64_t>(rendered.bytes.size()));
                    span->setAttribute("http.response.cache_control", "public, max-age=31536000, immutable");
                    span->setHttpStatus(200);
                }
                return response;
            });
    }

    ENDPOINT_INFO(promoteGeneratedMusic) {
        info->summary = "Promote a generated take to a permanent provenance-bearing WAV and attach it to its dialog";
        info->addTag("Multi-character Dialog");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/music/generated/{generationId}/promote", promoteGeneratedMusic,
             PATH(String, generationId), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/animation/dialog/music/generated/{generationId}/promote", "POST",
                           "api/v1/animation/dialog/music/generated/{generationId}/promote", "promoteGeneratedMusic",
                           "DialogMusicController", request,
                           [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               const std::string id = generationId ? std::string(*generationId) : std::string{};
                               if (!isUuidShape(id))
                                   return bailHttp(span, Status::CODE_400, "music generation id must be a UUID");
                               if (span)
                                   span->setAttribute("music.generation_id", id);
                               auto promoted = musicService_.promote(id, span);
                               if (!promoted.isSuccess())
                                   return bailFromServerError(span, promoted.getError().value());
                               if (span) {
                                   span->setHttpStatus(200);
                               }
                               return jsonResponse(span, Status::CODE_200,
                                                   api::dialogMusicPromotionResultToJson(promoted.getValue().value()));
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
