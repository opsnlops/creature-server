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

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/outgoing/ResponseFactory.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "api/DialogContracts.h"
#include "api/JobResponses.h"
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
#include "server/ws/dto/StatusDto.h"
#include "server/ws/service/DialogPreviewService.h"
#include "server/ws/service/SoundRenditionService.h"
#include "util/JsonParser.h"
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
    SoundRenditionService renditionService_;

    template <typename SpanT>
    Result<api::DialogPreviewRequest> parsePreviewRequest(const std::string &body, const SpanT &span,
                                                          std::string_view operation) {
        const auto parseSpan = creatures::observability
                                   ? creatures::observability->createChildOperationSpan(std::string(operation), span)
                                   : nullptr;
        if (parseSpan)
            parseSpan->setAttribute("validation.contract", "dialog.preview");
        const auto json = JsonParser::parseApiJsonString(body, "dialog preview request", parseSpan);
        if (!json.isSuccess()) {
            if (parseSpan)
                parseSpan->setAttribute("validation.result", "rejected");
            return Result<api::DialogPreviewRequest>{json.getError().value()};
        }
        const auto parsed = api::dialogPreviewRequestFromJson(json.getValue().value());
        if (!parsed.isSuccess()) {
            const auto error = parsed.getError().value();
            if (parseSpan)
                parseSpan->setAttribute("validation.result", "rejected");
            recordSpanError(parseSpan, error.getMessage(), "InvalidDialogPreviewRequest", error.getCode());
            return Result<api::DialogPreviewRequest>{error};
        }
        if (parseSpan) {
            parseSpan->setAttribute("validation.result", "accepted");
            parseSpan->setSuccess();
        }
        return parsed;
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
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/preview/meta", submitPreviewMeta,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/preview/meta", "POST", "api/v1/animation/dialog/preview/meta",
            "submitPreviewMeta", "DialogPreviewController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                const auto body = readRequestBodyLimited(request, api::MAX_DIALOG_REQUEST_BYTES, span);
                const auto parsed =
                    parsePreviewRequest(body, span, "DialogPreviewController.parseSubmitPreviewMetaRequest");
                if (!parsed.isSuccess())
                    return bailFromServerError(span, parsed.getError().value());
                auto opSpan = creatures::observability->createChildOperationSpan(
                    "DialogPreviewController.submitPreviewMeta", span);

                // Fast path: a specific generation_id, or the latest take when
                // regenerate is false, is a cheap disk read — serve it 200
                // synchronously. Anything requiring ElevenLabs becomes a job.
                const auto neutralRequest = parsed.getValue().value();
                auto fastResult = dialogPreviewService_.tryServeFromCache(neutralRequest, opSpan, "meta");
                if (!fastResult.isSuccess())
                    return bailFromServerError(span, fastResult.getError().value());
                const auto fast = fastResult.getValue().value();

                if (fast.cacheHit) {
                    const auto response = DialogPreviewService::makeMetaResponse(
                        fast.outcome->generation, fast.outcome->cacheKey, fast.outcome->cached);
                    if (span)
                        span->setHttpStatus(200);
                    return jsonResponse(span, Status::CODE_200, api::dialogPreviewMetaResponseToJson(response));
                }

                // Generation needed — hand off to the JobWorker and return 202.
                const auto detailsStr = api::dialogPreviewRequestToJson(neutralRequest).dump();
                const auto admission = creatures::jobWorker->tryCreateAndQueueJob(
                    creatures::jobs::JobType::DialogPreview, detailsStr, span);
                if (admission.status == creatures::jobs::JobWorker::QueueAdmission::Status::Full) {
                    return bailHttp(span, Status::CODE_429,
                                    "Eight dialog jobs are already queued or running; try again shortly", nullptr,
                                    "QueueAdmissionRejected");
                }
                if (admission.status == creatures::jobs::JobWorker::QueueAdmission::Status::EnqueueFailed) {
                    return bailHttp(span, Status::CODE_500, "Could not queue dialog preview job", nullptr,
                                    "QueueEnqueueFailure");
                }
                const auto &jobId = admission.jobId;
                if (span) {
                    span->setAttribute("job.id", jobId);
                    span->setHttpStatus(202);
                }
                const api::JobCreatedResponse response{
                    jobId, "dialog-preview",
                    "Dialog preview job created. Listen for job-progress and job-complete WebSocket messages on "
                    "this job_id, or poll GET /api/v1/job/{job_id}."};
                return jsonResponse(span, Status::CODE_202, api::jobCreatedResponseToJson(response));
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
                if (!api::isLowercaseSha256(ck))
                    return bailHttp(span, Status::CODE_400, "cache_key must be a 64-character lowercase hex sha256");
                if (!isUuidShape(gid))
                    return bailHttp(span, Status::CODE_400, "generation_id must be a UUID");
                auto loadResult = creatures::voice::loadGeneration(ck, gid);
                if (!loadResult.isSuccess()) {
                    return bailHttp(span, Status::CODE_404, fmt::format("generation '{}/{}' not found", ck, gid));
                }
                const auto gen = loadResult.getValue().value();
                // Embed provenance (#50). Drop the track list — this is a mono
                // file, so a 17-track layout would misdescribe it; the script text
                // and ids still travel.
                auto monoProv = gen.provenance;
                monoProv.tracks.clear();
                const auto wavBytes =
                    creatures::voice::wrapMonoPcmAsWav(gen.audioPcm, 48000, monoProv.empty() ? nullptr : &monoProv);

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
        info->summary = "Download a shareable MP3 or Ogg/Opus version of a cached dialog generation";
        info->description = "Encodes the cached mono PCM for {cache_key}/{generation_id} to a shareable rendition "
                            "(mono, 128 kbps) for sharing. The trailing extension of the last path segment picks the "
                            "format: '.mp3' → audio/mpeg (plays in Slack + AVFoundation; issue #58), '.ogg' (or no "
                            "extension) → audio/ogg. URL is built from the cache_key/generation_id of a /preview/meta "
                            "response. 404 if the generation isn't cached (never existed or has been cron-swept).";
        info->addTag("Multi-character Dialog");
        info->pathParams["cache_key"].description = "Hex sha256 of the turns; from /preview/meta or /preview/lookup.";
        info->pathParams["generation_id"].description = "UUID of the specific take; from /preview/meta or /lookup.";
        info->addResponse<oatpp::String>(Status::CODE_200, "audio/mpeg");
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

                // The trailing extension of the last segment picks the rendition format:
                // '.mp3' → MP3 (issue #58), '.ogg' or no extension → Ogg/Opus. Strip it
                // to recover the bare generation_id.
                bool wantMp3 = false;
                if (gid.size() > 4 && gid.compare(gid.size() - 4, 4, ".mp3") == 0) {
                    wantMp3 = true;
                    gid.resize(gid.size() - 4);
                } else if (gid.size() > 4 && gid.compare(gid.size() - 4, 4, ".ogg") == 0) {
                    gid.resize(gid.size() - 4);
                }
                if (!api::isLowercaseSha256(ck))
                    return bailHttp(span, Status::CODE_400, "cache_key must be a 64-character lowercase hex sha256");
                if (!isUuidShape(gid))
                    return bailHttp(span, Status::CODE_400, "generation_id must be a UUID");
                auto loadResult = creatures::voice::loadGeneration(ck, gid);
                if (!loadResult.isSuccess()) {
                    return bailHttp(span, Status::CODE_404, fmt::format("generation '{}/{}' not found", ck, gid));
                }
                const auto gen = loadResult.getValue().value();

                // The cache stores raw S16LE bytes; the encoder wants samples.
                std::vector<int16_t> samples(gen.audioPcm.size() / sizeof(int16_t));
                std::memcpy(samples.data(), gen.audioPcm.data(), samples.size() * sizeof(int16_t));

                auto renditionSpan =
                    creatures::observability->createChildOperationSpan("SoundRenditionService.renderMonoPcm", span);
                if (renditionSpan) {
                    renditionSpan->setAttribute("dialog.cache_key", ck);
                    renditionSpan->setAttribute("dialog.generation_id", gid);
                    renditionSpan->setAttribute("rendition.format", wantMp3 ? "mp3" : "ogg_opus");
                    renditionSpan->setAttribute("rendition.input_samples", static_cast<int64_t>(samples.size()));
                    renditionSpan->setAttribute("cache.outcome", "bypass");
                    renditionSpan->setAttribute("encoding.performed", true);
                    renditionSpan->setAttribute("encoding.input_bytes",
                                                static_cast<int64_t>(samples.size() * sizeof(int16_t)));
                }
                auto rendition = renditionService_.renderMonoPcm(samples, 48000, gen.provenance,
                                                                 wantMp3 ? SoundRenditionFormat::Mp3
                                                                         : SoundRenditionFormat::OggOpus);
                if (!rendition.isSuccess()) {
                    recordSpanError(renditionSpan, rendition.getError().value().getMessage(), "RenditionError",
                                    rendition.getError().value().getCode());
                    return bailFromServerError(span, rendition.getError().value());
                }
                const auto rendered = rendition.getValue().value();
                if (renditionSpan) {
                    renditionSpan->setAttribute("rendition.output_bytes", static_cast<int64_t>(rendered.bytes.size()));
                    renditionSpan->setSuccess();
                }
                const auto shareName = fmt::format("dialog-preview-{}{}", gid.substr(0, 8), rendered.extension);

                auto response = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
                    Status::CODE_200, oatpp::String(reinterpret_cast<const char *>(rendered.bytes.data()),
                                                    static_cast<v_int32>(rendered.bytes.size())));
                response->putHeader("Content-Type", rendered.mimeType.c_str());
                response->putHeader("Content-Disposition", "attachment; filename=\"" + shareName + "\"");
                response->putHeader("Cache-Control", "public, max-age=31536000, immutable");
                response->putHeader("X-Dialog-Cache-Key", ck.c_str());
                response->putHeader("X-Dialog-Generation-Id", gid.c_str());
                if (span) {
                    span->setAttribute("dialog.cache_key", ck);
                    span->setAttribute("dialog.generation_id", gid);
                    span->setAttribute("rendition.format", wantMp3 ? "mp3" : "ogg");
                    span->setAttribute("share.bytes", static_cast<int64_t>(rendered.bytes.size()));
                    span->setAttribute("http.response.cache_control", "public, max-age=31536000, immutable");
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
        info->addResponse<oatpp::String>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/preview/multichannel", submitPreviewMultichannel,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/preview/multichannel", "POST",
            "api/v1/animation/dialog/preview/multichannel", "submitPreviewMultichannel", "DialogPreviewController",
            request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                const auto body = readRequestBodyLimited(request, api::MAX_DIALOG_REQUEST_BYTES, span);
                const auto parsed =
                    parsePreviewRequest(body, span, "DialogPreviewController.parseSubmitPreviewMultichannelRequest");
                if (!parsed.isSuccess())
                    return bailFromServerError(span, parsed.getError().value());

                // Always a job. Even a fully-cached long scene means writing a
                // ~0.5 GB 17-channel WAV, which must never ride one HTTP
                // response. The worker generates/loads the take, assembles the
                // WAV into the ad-hoc bucket, and reports a downloadable
                // file_name in the completion result.
                const auto detailsStr = api::dialogPreviewRequestToJson(parsed.getValue().value()).dump();
                const auto admission = creatures::jobWorker->tryCreateAndQueueJob(
                    creatures::jobs::JobType::DialogPreviewExport, detailsStr, span);
                if (admission.status == creatures::jobs::JobWorker::QueueAdmission::Status::Full) {
                    return bailHttp(span, Status::CODE_429,
                                    "Eight dialog jobs are already queued or running; try again shortly", nullptr,
                                    "QueueAdmissionRejected");
                }
                if (admission.status == creatures::jobs::JobWorker::QueueAdmission::Status::EnqueueFailed) {
                    return bailHttp(span, Status::CODE_500, "Could not queue dialog preview export job", nullptr,
                                    "QueueEnqueueFailure");
                }
                const auto &jobId = admission.jobId;
                if (span) {
                    span->setAttribute("job.id", jobId);
                    span->setHttpStatus(202);
                }
                const api::JobCreatedResponse response{
                    jobId, "dialog-preview-export",
                    "Dialog preview export job created. The 17-channel WAV lands in the ad-hoc sound bucket; "
                    "the completion result carries its file_name (downloadable via GET "
                    "/api/v1/sound/ad-hoc/{filename}). Listen for job-complete on this job_id, or poll GET "
                    "/api/v1/job/{job_id}."};
                return jsonResponse(span, Status::CODE_202, api::jobCreatedResponseToJson(response));
            });
    }

    ENDPOINT_INFO(lookupPreview) {
        info->summary = "Check what dialog generations are cached for a given input";
        info->description =
            "Cheap cache-lookup endpoint — does no audio work. Returns the list of cached generations (newest "
            "first) for the given turns, or 404 if nothing is cached. UI can use this to badge the 'Make "
            "Animation' button as fast (cached) vs slow (will hit ElevenLabs).";
        info->addTag("Multi-character Dialog");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/preview/lookup", lookupPreview,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/preview/lookup", "POST", "api/v1/animation/dialog/preview/lookup",
            "lookupPreview", "DialogPreviewController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                const auto body = readRequestBodyLimited(request, api::MAX_DIALOG_REQUEST_BYTES, span);
                const auto parseSpan = creatures::observability
                                           ? creatures::observability->createChildOperationSpan(
                                                 "DialogPreviewController.parseLookupPreviewRequest", span)
                                           : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "dialog.preview.lookup");
                const auto json = JsonParser::parseApiJsonString(body, "dialog preview lookup request", parseSpan);
                if (!json.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, json.getError().value());
                }
                const auto parsed = api::dialogPreviewLookupRequestFromJson(json.getValue().value());
                if (!parsed.isSuccess()) {
                    const auto error = parsed.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidDialogPreviewLookupRequest",
                                    error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                auto opSpan =
                    creatures::observability->createChildOperationSpan("DialogPreviewController.lookupPreview", span);
                const auto turns = parsed.getValue().value();
                auto resolvedResult = DialogPreviewService::resolveCreatures(turns, opSpan);
                if (!resolvedResult.isSuccess()) {
                    return bailFromServerError(span, resolvedResult.getError().value());
                }
                const auto inputs = DialogPreviewService::buildDialogInputs(turns, resolvedResult.getValue().value());
                const auto cacheKey = creatures::voice::computeCacheKey(inputs);
                const auto generations = creatures::voice::listGenerations(cacheKey);

                if (generations.empty()) {
                    if (span) {
                        span->setAttribute("dialog.cache_key", cacheKey);
                    }
                    return bailHttp(span, Status::CODE_404, "no cached generations for these turns");
                }

                api::DialogPreviewLookupResponse response;
                response.cacheKey = cacheKey;
                response.latestGenerationId = generations.front().generationId;
                response.generations.reserve(generations.size());
                for (const auto &g : generations) {
                    // ISO-8601 from the time_point.
                    const auto secs =
                        std::chrono::duration_cast<std::chrono::seconds>(g.createdAt.time_since_epoch()).count();
                    const std::time_t tt = static_cast<std::time_t>(secs);
                    std::tm tm{};
                    gmtime_r(&tt, &tm);
                    response.generations.push_back(
                        {g.generationId, fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", tm.tm_year + 1900,
                                                     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec)});
                }
                if (span) {
                    span->setAttribute("dialog.cache_key", cacheKey);
                    span->setAttribute("dialog.generations", static_cast<int64_t>(generations.size()));
                    span->setHttpStatus(200);
                }
                return jsonResponse(span, Status::CODE_200, api::dialogPreviewLookupResponseToJson(response));
            });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
