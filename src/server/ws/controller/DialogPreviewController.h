#pragma once

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <base64.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/outgoing/ResponseFactory.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "server/audio/OggOpusWriter.h"
#include "server/database.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/namespace-stuffs.h"
#include "server/voice/DialogCache.h"
#include "server/voice/DialogClient.h"
#include "server/voice/DialogPipeline.h"
#include "server/voice/DialogPreviewAssembly.h"
#include "server/voice/DialogWav.h"
#include "server/voice/PcmWavWriter.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/dto/DialogDto.h"
#include "server/ws/dto/JobCreatedDto.h"
#include "server/ws/dto/StatusDto.h"
#include "server/ws/service/DialogPreviewService.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures::ws {

/// HTTP surface for the dialog preview / cache (companion to DialogController).
///
/// Two endpoints:
///   * POST /api/v1/animation/dialog/preview        — generate/return a take
///   * POST /api/v1/animation/dialog/preview/lookup — check what's cached
///
/// The preview path returns either JSON (mono format, default) or audio/wav
/// bytes (multichannel format, for downloading + inspecting in Audacity).
/// Both share the same cache lookup; the multichannel branch additionally
/// runs the per-creature slice + 17-channel WAV assembly.
class DialogPreviewController : public oatpp::web::server::api::ApiController,
                                public HttpResponseHelpers<DialogPreviewController> {
  public:
    DialogPreviewController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : ApiController(objectMapper) {}

    static std::shared_ptr<DialogPreviewController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                                 objectMapper)) {
        return std::make_shared<DialogPreviewController>(objectMapper);
    }

  private:
    /// The HTTP-free pipeline shared with the JobWorker. The controller drives
    /// only the cheap cache fast-path + job creation; all generation/assembly
    /// lives in the service.
    DialogPreviewService dialogPreviewService_;

    /// Serialize a preview request DTO into the job framework's string-typed
    /// `details` field. The worker round-trips it back through the same
    /// ObjectMapper so both sides share one schema.
    static std::string serializePreviewRequest(const oatpp::Object<DialogPreviewRequestDto> &body) {
        auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
        return jsonMapper->writeToString(body)->c_str();
    }

    /// Shared validation for the two POST preview endpoints (/meta and
    /// /multichannel). Returns nullptr on success; an error response otherwise.
    template <typename SpanT>
    std::shared_ptr<OutgoingResponse> validatePreviewBody(const oatpp::Object<DialogPreviewRequestDto> &body,
                                                          const SpanT &span) {
        if (!body || !body->turns || body->turns->empty()) {
            return bailHttp(span, Status::CODE_400, "turns must be a non-empty array");
        }
        for (const auto &t : *body->turns) {
            if (!t || !t->creature_id || t->creature_id->empty() || !t->text || t->text->empty()) {
                return bailHttp(span, Status::CODE_400, "every turn must have a non-empty creature_id and text");
            }
        }
        return nullptr;
    }

  public:
    ENDPOINT_INFO(submitPreviewMeta) {
        info->summary = "Generate (or load) a dialog preview and return its metadata + audio URL";
        info->description =
            "Returns small JSON: cache_key, generation_id, cached flag, audio_url (GET this for the mono WAV "
            "ready for an <audio> element), audio_format, sample_rate, duration_seconds, voice_segments, and "
            "forced-alignment word/char timings. Generates fresh via ElevenLabs if no cache hit (or "
            "regenerate=true); reuses the latest cached take by default; loads a specific take if generation_id "
            "is set (404 if expired).";
        info->addTag("Multi-character Dialog");
        info->addResponse<Object<DialogPreviewMetaResponseDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<JobCreatedDto>>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/preview/meta", submitPreviewMeta,
             BODY_DTO(Object<DialogPreviewRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/preview/meta", "POST", "api/v1/animation/dialog/preview/meta",
            "submitPreviewMeta", "DialogPreviewController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (auto errResp = validatePreviewBody(requestBody, span))
                    return errResp;
                auto opSpan = creatures::observability->createChildOperationSpan(
                    "DialogPreviewController.submitPreviewMeta", span);

                // Fast path: a specific generation_id, or the latest take when
                // regenerate is false, is a cheap disk read — serve it 200
                // synchronously. Anything requiring ElevenLabs becomes a job.
                auto fastResult = dialogPreviewService_.tryServeFromCache(requestBody, opSpan, "meta");
                if (!fastResult.isSuccess())
                    return bailFromServerError(span, fastResult.getError().value());
                const auto fast = fastResult.getValue().value();

                if (fast.cacheHit) {
                    auto dto = DialogPreviewMetaResponseDto::createShared();
                    DialogPreviewService::populateMetaResponse(dto, fast.outcome->generation, fast.outcome->cacheKey,
                                                               fast.outcome->cached);
                    if (span)
                        span->setHttpStatus(200);
                    return createDtoResponse(Status::CODE_200, dto);
                }

                // Generation needed — hand off to the JobWorker and return 202.
                std::string detailsStr;
                try {
                    detailsStr = serializePreviewRequest(requestBody);
                } catch (const std::exception &e) {
                    if (span)
                        span->setError(e.what());
                    return bailHttp(span, Status::CODE_500,
                                    fmt::format("failed to serialize request body: {}", e.what()));
                }
                const std::string jobId =
                    creatures::jobManager->createJob(creatures::jobs::JobType::DialogPreview, detailsStr, span);
                creatures::jobWorker->queueJob(jobId);
                if (span) {
                    span->setAttribute("job.id", jobId);
                    span->setHttpStatus(202);
                }
                auto response = JobCreatedDto::createShared();
                response->job_id = jobId.c_str();
                response->job_type = "dialog-preview";
                response->message = "Dialog preview job created. Listen for job-progress and job-complete "
                                    "WebSocket messages on this job_id, or poll GET /api/v1/job/{job_id}.";
                return createDtoResponse(Status::CODE_202, response);
            });
    }

    ENDPOINT_INFO(getPreviewAudio) {
        info->summary = "Stream the mono WAV for a cached dialog generation";
        info->description = "Reads the cached mono PCM for {cache_key}/{generation_id}, wraps it in a 44-byte "
                            "PCM WAV header on the fly, and streams audio/wav back. URL is built from the "
                            "audio_url field of a /preview/meta response. 404 if the generation isn't cached "
                            "(never existed or has been cron-swept).";
        info->addTag("Multi-character Dialog");
        info->pathParams["cache_key"].description = "Hex sha256 of the turns; from /preview/meta or /preview/lookup.";
        info->pathParams["generation_id"].description = "UUID of the specific take; from /preview/meta or /lookup.";
        info->addResponse<oatpp::String>(Status::CODE_200, "audio/wav");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    // oatpp's URL Pattern matcher only checks parts at `/` boundaries — a
    // literal suffix after a path variable (`{var}.wav`) is parsed but never
    // matches. So we make the whole last segment one variable (`{filename}`)
    // and strip the `.wav` server-side.
    ENDPOINT("GET", "api/v1/animation/dialog/preview/audio/{cache_key}/{filename}", getPreviewAudio,
             PATH(String, cache_key), PATH(String, filename), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/animation/dialog/preview/audio/{cache_key}/{filename}", "GET",
            "api/v1/animation/dialog/preview/audio/{cache_key}/{filename}", "getPreviewAudio",
            "DialogPreviewController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                const std::string ck = cache_key ? std::string(*cache_key) : std::string();
                std::string gid = filename ? std::string(*filename) : std::string();
                // Accept either {id} or {id}.wav (preferred for
                // browser save-as / Content-Type sniffing).
                if (gid.size() > 4 && gid.compare(gid.size() - 4, 4, ".wav") == 0) {
                    gid.resize(gid.size() - 4);
                }
                if (ck.empty() || gid.empty()) {
                    return bailHttp(span, Status::CODE_400, "cache_key and generation_id are required");
                }
                auto loadResult = creatures::voice::loadGeneration(ck, gid);
                if (!loadResult.isSuccess()) {
                    return bailHttp(span, Status::CODE_404, fmt::format("generation '{}/{}' not found", ck, gid));
                }
                const auto gen = loadResult.getValue().value();
                const auto wavBytes = creatures::voice::wrapMonoPcmAsWav(gen.audioPcm, 48000);

                auto response = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
                    Status::CODE_200, oatpp::String(reinterpret_cast<const char *>(wavBytes.data()),
                                                    static_cast<v_int32>(wavBytes.size())));
                response->putHeader("Content-Type", "audio/wav");
                response->putHeader("X-Dialog-Cache-Key", ck.c_str());
                response->putHeader("X-Dialog-Generation-Id", gid.c_str());
                if (span) {
                    span->setAttribute("dialog.cache_key", ck);
                    span->setAttribute("dialog.generation_id", gid);
                    span->setHttpStatus(200);
                }
                return response;
            });
    }

    ENDPOINT_INFO(getPreviewShareable) {
        info->summary = "Download a shareable Ogg/Opus version of a cached dialog generation";
        info->description = "Encodes the cached mono PCM for {cache_key}/{generation_id} to Ogg/Opus (96 kbps, "
                            "mono) for sharing. URL is built from the cache_key/generation_id of a /preview/meta "
                            "response. 404 if the generation isn't cached (never existed or has been cron-swept).";
        info->addTag("Multi-character Dialog");
        info->pathParams["cache_key"].description = "Hex sha256 of the turns; from /preview/meta or /preview/lookup.";
        info->pathParams["generation_id"].description = "UUID of the specific take; from /preview/meta or /lookup.";
        info->addResponse<oatpp::String>(Status::CODE_200, "audio/ogg");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    // Same URL-matcher caveat as getPreviewAudio: the whole last segment is one
    // variable and the extension is stripped server-side.
    ENDPOINT("GET", "api/v1/animation/dialog/preview/share/{cache_key}/{filename}", getPreviewShareable,
             PATH(String, cache_key), PATH(String, filename), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/animation/dialog/preview/share/{cache_key}/{filename}", "GET",
            "api/v1/animation/dialog/preview/share/{cache_key}/{filename}", "getPreviewShareable",
            "DialogPreviewController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                const std::string ck = cache_key ? std::string(*cache_key) : std::string();
                std::string gid = filename ? std::string(*filename) : std::string();
                if (gid.size() > 4 && gid.compare(gid.size() - 4, 4, ".ogg") == 0) {
                    gid.resize(gid.size() - 4);
                }
                if (ck.empty() || gid.empty()) {
                    return bailHttp(span, Status::CODE_400, "cache_key and generation_id are required");
                }
                auto loadResult = creatures::voice::loadGeneration(ck, gid);
                if (!loadResult.isSuccess()) {
                    return bailHttp(span, Status::CODE_404, fmt::format("generation '{}/{}' not found", ck, gid));
                }
                const auto gen = loadResult.getValue().value();

                // The cache stores raw S16LE bytes; the encoder wants samples.
                std::vector<int16_t> samples(gen.audioPcm.size() / sizeof(int16_t));
                std::memcpy(samples.data(), gen.audioPcm.data(), samples.size() * sizeof(int16_t));

                auto oggResult = creatures::audio::encodeMonoToOggOpus(samples, creatures::audio::kShareableSampleRate);
                if (!oggResult.isSuccess()) {
                    return bailHttp(span, Status::CODE_500, oggResult.getError().value().getMessage());
                }
                const auto oggBytes = oggResult.getValue().value();
                const auto shareName = fmt::format("dialog-preview-{}.ogg", gid.substr(0, 8));

                auto response = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
                    Status::CODE_200, oatpp::String(reinterpret_cast<const char *>(oggBytes.data()),
                                                    static_cast<v_int32>(oggBytes.size())));
                response->putHeader("Content-Type", "audio/ogg");
                response->putHeader("Content-Disposition", "attachment; filename=\"" + shareName + "\"");
                response->putHeader("X-Dialog-Cache-Key", ck.c_str());
                response->putHeader("X-Dialog-Generation-Id", gid.c_str());
                if (span) {
                    span->setAttribute("dialog.cache_key", ck);
                    span->setAttribute("dialog.generation_id", gid);
                    span->setAttribute("share.bytes", static_cast<int64_t>(oggBytes.size()));
                    span->setHttpStatus(200);
                }
                return response;
            });
    }

    ENDPOINT_INFO(submitPreviewMultichannel) {
        info->summary = "Generate (or load) a dialog preview and return the assembled 17-channel WAV";
        info->description = "Same cache semantics as /preview/meta (use generation_id / regenerate flags the same "
                            "way). Returns audio/wav bytes — the 17-channel WAV the show would play. Suitable for "
                            "downloading into Audacity (or any 17-channel-aware tool) for inspection. Each "
                            "creature's audio appears in its `audio_channel` lane; all other lanes are silent.";
        info->addTag("Multi-character Dialog");
        info->addResponse<Object<JobCreatedDto>>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/preview/multichannel", submitPreviewMultichannel,
             BODY_DTO(Object<DialogPreviewRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/preview/multichannel", "POST",
            "api/v1/animation/dialog/preview/multichannel", "submitPreviewMultichannel", "DialogPreviewController",
            request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (auto errResp = validatePreviewBody(requestBody, span))
                    return errResp;

                // Always a job. Even a fully-cached long scene means writing a
                // ~0.5 GB 17-channel WAV, which must never ride one HTTP
                // response. The worker generates/loads the take, assembles the
                // WAV into the ad-hoc bucket, and reports a downloadable
                // file_name in the completion result.
                std::string detailsStr;
                try {
                    detailsStr = serializePreviewRequest(requestBody);
                } catch (const std::exception &e) {
                    if (span)
                        span->setError(e.what());
                    return bailHttp(span, Status::CODE_500,
                                    fmt::format("failed to serialize request body: {}", e.what()));
                }
                const std::string jobId =
                    creatures::jobManager->createJob(creatures::jobs::JobType::DialogPreviewExport, detailsStr, span);
                creatures::jobWorker->queueJob(jobId);
                if (span) {
                    span->setAttribute("job.id", jobId);
                    span->setHttpStatus(202);
                }
                auto response = JobCreatedDto::createShared();
                response->job_id = jobId.c_str();
                response->job_type = "dialog-preview-export";
                response->message =
                    "Dialog preview export job created. The 17-channel WAV lands in the ad-hoc sound bucket; "
                    "the completion result carries its file_name (downloadable via GET "
                    "/api/v1/sound/ad-hoc/{filename}). Listen for job-complete on this job_id, or poll GET "
                    "/api/v1/job/{job_id}.";
                return createDtoResponse(Status::CODE_202, response);
            });
    }

    ENDPOINT_INFO(lookupPreview) {
        info->summary = "Check what dialog generations are cached for a given input";
        info->description =
            "Cheap cache-lookup endpoint — does no audio work. Returns the list of cached generations (newest "
            "first) for the given turns, or 404 if nothing is cached. UI can use this to badge the 'Make "
            "Animation' button as fast (cached) vs slow (will hit ElevenLabs).";
        info->addTag("Multi-character Dialog");
        info->addResponse<Object<DialogPreviewLookupResponseDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/preview/lookup", lookupPreview,
             BODY_DTO(Object<DialogPreviewLookupRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/preview/lookup", "POST", "api/v1/animation/dialog/preview/lookup",
            "lookupPreview", "DialogPreviewController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!requestBody || !requestBody->turns || requestBody->turns->empty()) {
                    return bailHttp(span, Status::CODE_400, "turns must be a non-empty array");
                }
                for (const auto &t : *requestBody->turns) {
                    if (!t || !t->creature_id || t->creature_id->empty() || !t->text || t->text->empty()) {
                        return bailHttp(span, Status::CODE_400,
                                        "every turn must have a non-empty creature_id and text");
                    }
                }

                auto opSpan =
                    creatures::observability->createChildOperationSpan("DialogPreviewController.lookupPreview", span);
                auto resolvedResult = DialogPreviewService::resolveCreatures(requestBody->turns, opSpan);
                if (!resolvedResult.isSuccess()) {
                    return bailFromServerError(span, resolvedResult.getError().value());
                }
                const auto inputs =
                    DialogPreviewService::buildDialogInputs(requestBody->turns, resolvedResult.getValue().value());
                const auto cacheKey = creatures::voice::computeCacheKey(inputs);
                const auto generations = creatures::voice::listGenerations(cacheKey);

                if (generations.empty()) {
                    if (span) {
                        span->setAttribute("dialog.cache_key", cacheKey);
                    }
                    return bailHttp(span, Status::CODE_404, "no cached generations for these turns");
                }

                auto dto = DialogPreviewLookupResponseDto::createShared();
                dto->cache_key = cacheKey.c_str();
                dto->latest_generation_id = generations.front().generationId.c_str();
                auto entries = oatpp::List<oatpp::Object<DialogPreviewGenerationEntryDto>>::createShared();
                for (const auto &g : generations) {
                    auto ed = DialogPreviewGenerationEntryDto::createShared();
                    ed->generation_id = g.generationId.c_str();
                    // ISO-8601 from the time_point.
                    const auto secs =
                        std::chrono::duration_cast<std::chrono::seconds>(g.createdAt.time_since_epoch()).count();
                    const std::time_t tt = static_cast<std::time_t>(secs);
                    std::tm tm{};
                    gmtime_r(&tt, &tm);
                    ed->created_at = fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", tm.tm_year + 1900,
                                                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec)
                                         .c_str();
                    entries->push_back(ed);
                }
                dto->generations = entries;
                if (span) {
                    span->setAttribute("dialog.cache_key", cacheKey);
                    span->setAttribute("dialog.generations", static_cast<int64_t>(generations.size()));
                    span->setHttpStatus(200);
                }
                return createDtoResponse(Status::CODE_200, dto);
            });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
