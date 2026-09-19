#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>

#include <fmt/format.h>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/outgoing/ResponseFactory.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "api/JobResponses.h"
#include "api/JsonResponse.h"
#include "api/MusicContracts.h"
#include "model/MusicPiece.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/controller/MusicCandidateMp3.h"
#include "server/ws/service/DialogMusicService.h"
#include "server/ws/service/MusicService.h"
#include "server/ws/service/SoundRenditionService.h"
#include "util/JsonParser.h"
#include "util/Slugify.h"
#include "util/helpers.h"

namespace creatures {
extern std::shared_ptr<jobs::JobWorker> jobWorker;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures::ws {

/// The music library (#202): dialog-free generation, candidates, and saved
/// pieces with versions. Candidate routes are aliases of the dialog music
/// ones so the console has one base path for library work.
class MusicController : public oatpp::web::server::api::ApiController, public HttpResponseHelpers<MusicController> {
  public:
    MusicController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) : ApiController(objectMapper) {}

    static std::shared_ptr<MusicController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) {
        return std::make_shared<MusicController>(objectMapper);
    }

  private:
    MusicService musicService_;
    DialogMusicService dialogMusicService_; // recipe(): same candidate cache, same shape
    SoundRenditionService renditionService_;
    std::mutex renditionMutex_;

    /// Body → JSON → contract, with the validation span every parser gets.
    /// Returns nullopt after a bail response has been prepared in `failure`.
    template <typename Parser, typename SpanT>
    auto parseBody(const std::shared_ptr<IncomingRequest> &request, const char *contract, const char *what,
                   Parser &&parser, const SpanT &span, std::shared_ptr<OutgoingResponse> &failure)
        -> std::optional<std::decay_t<decltype(parser(nlohmann::json{}).getValue().value())>> {
        const auto body = readRequestBodyLimited(request, api::MAX_MUSIC_REQUEST_BYTES, span);
        const auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                              fmt::format("MusicController.parse.{}", contract), span)
                                                        : nullptr;
        if (parseSpan)
            parseSpan->setAttribute("validation.contract", contract);
        const auto json = JsonParser::parseApiJsonString(body, what, parseSpan);
        if (!json.isSuccess()) {
            if (parseSpan)
                parseSpan->setAttribute("validation.result", "rejected");
            failure = bailFromServerError(span, json.getError().value());
            return std::nullopt;
        }
        auto parsed = parser(json.getValue().value());
        if (!parsed.isSuccess()) {
            const auto error = parsed.getError().value();
            if (parseSpan)
                parseSpan->setAttribute("validation.result", "rejected");
            recordSpanError(parseSpan, error.getMessage(), "InvalidMusicRequest", error.getCode());
            failure = bailFromServerError(span, error);
            return std::nullopt;
        }
        if (parseSpan) {
            parseSpan->setAttribute("validation.result", "accepted");
            parseSpan->setSuccess();
        }
        return parsed.getValue().value();
    }

    template <typename SpanT>
    std::shared_ptr<OutgoingResponse> pieceResponse(const SpanT &span, const Status &status, const MusicPiece &piece) {
        if (span) {
            span->setAttribute("music.piece_id", piece.id);
            span->setAttribute("music.version_count", static_cast<int64_t>(piece.versions.size()));
            span->setHttpStatus(status.code);
        }
        return jsonResponse(span, status, musicPieceToJson(piece));
    }

  public:
    ENDPOINT_INFO(generateMusic) {
        info->summary = "Generate a piece of music that is not bound to a dialog";
        info->description =
            "Returns a job id; progress/completion arrive on the job WebSocket. Prompt mode needs `music_length_ms`; "
            "plan mode sends `composition_plan`; sections mode sends editable `sections` and optionally keeps some "
            "of them from a piece's version (`piece_id` + `base_version_id` + `keep`), which is how a piece is "
            "refined without throwing the rest away. See docs/music-library-plan.md.";
        info->addTag("Music Library");
        info->addResponse<oatpp::String>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_429, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/music/generate", generateMusic, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/music/generate", "POST", "api/v1/music/generate", "generateMusic", "MusicController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                std::shared_ptr<OutgoingResponse> failure;
                auto parsed = parseBody(request, "music.generate", "music generate request",
                                        api::musicGenerateRequestFromJson, span, failure);
                if (!parsed)
                    return failure;
                const auto &musicRequest = *parsed;
                if (span) {
                    span->setAttribute("music.request_kind", musicRequest.requestKind());
                    span->setAttribute("music.model_id", musicRequest.modelId);
                    if (!musicRequest.pieceId.empty())
                        span->setAttribute("music.piece_id", musicRequest.pieceId);
                    if (musicRequest.sections) {
                        span->setAttribute("music.section_count", static_cast<int64_t>(musicRequest.sections->size()));
                        span->setAttribute("music.kept_count", static_cast<int64_t>(musicRequest.keep.size()));
                    }
                }
                const auto admission = creatures::jobWorker->tryCreateAndQueueMusicJob(
                    api::musicGenerateRequestToJson(musicRequest).dump(), span, creatures::jobs::JobType::Music);
                if (admission.status == creatures::jobs::JobWorker::QueueAdmission::Status::Full)
                    return bailHttp(span, Status::CODE_429,
                                    "Two music generations are already queued or running; try again shortly", nullptr,
                                    "QueueAdmissionRejected");
                if (admission.status == creatures::jobs::JobWorker::QueueAdmission::Status::EnqueueFailed)
                    return bailHttp(span, Status::CODE_500, "Could not queue music job", nullptr,
                                    "QueueEnqueueFailure");
                if (span) {
                    span->setAttribute("job.id", admission.jobId);
                    span->setHttpStatus(202);
                }
                const api::JobCreatedResponse response{
                    admission.jobId, "music", "Music job created; listen for job-progress and job-complete messages."};
                return jsonResponse(span, Status::CODE_202, api::jobCreatedResponseToJson(response));
            });
    }

    ENDPOINT_INFO(getMusicCandidateMp3) {
        info->summary = "Get the immutable MP3 audition rendition of a music candidate";
        info->addTag("Music Library");
        info->addResponse<String>(Status::CODE_200, "audio/mpeg");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/music/generated/{filename}", getMusicCandidateMp3, PATH(String, filename),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/music/generated/{filename}", "GET", "api/v1/music/generated/{filename}",
            "getMusicCandidateMp3", "MusicController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                std::string value = filename ? std::string(*filename) : std::string{};
                if (!value.ends_with(".mp3"))
                    return bailHttp(span, Status::CODE_422, "generated music URLs must end in .mp3");
                value.resize(value.size() - 4);
                if (!isUuidShape(value))
                    return bailHttp(span, Status::CODE_400, "music generation id must be a UUID");
                if (span)
                    span->setAttribute("music.generation_id", value);
                auto rendition = renderMusicCandidateMp3(value, renditionService_, renditionMutex_, span);
                if (!rendition.isSuccess())
                    return bailFromServerError(span, rendition.getError().value());
                const auto rendered = rendition.getValue().value();
                const auto downloadName = util::musicExportBasename(rendered.generation.title, value) + ".mp3";
                auto response = ResponseFactory::createResponse(
                    Status::CODE_200, oatpp::String(reinterpret_cast<const char *>(rendered.bytes.data()),
                                                    static_cast<v_buff_size>(rendered.bytes.size())));
                response->putHeader("Content-Type", rendered.mimeType.c_str());
                response->putHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
                response->putHeader("Cache-Control", "public, max-age=31536000, immutable");
                response->putHeader("X-Music-Generation-Id", value.c_str());
                if (span) {
                    span->setAttribute("rendition.bytes", static_cast<int64_t>(rendered.bytes.size()));
                    span->setHttpStatus(200);
                }
                return response;
            });
    }

    ENDPOINT_INFO(getMusicCandidateRecipe) {
        info->summary = "Get the generation controls a music candidate was made with";
        info->addTag("Music Library");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/music/generated/{generationId}/recipe", getMusicCandidateRecipe, PATH(String, generationId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/music/generated/{generationId}/recipe", "GET",
                           "api/v1/music/generated/{generationId}/recipe", "getMusicCandidateRecipe", "MusicController",
                           request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               const std::string id = generationId ? std::string(*generationId) : std::string{};
                               if (!isUuidShape(id))
                                   return bailHttp(span, Status::CODE_400, "music generation id must be a UUID");
                               if (span)
                                   span->setAttribute("music.generation_id", id);
                               auto recipe = dialogMusicService_.recipe(id, span);
                               if (!recipe.isSuccess())
                                   return bailFromServerError(span, recipe.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(span, Status::CODE_200,
                                                   api::dialogMusicRecipeToJson(recipe.getValue().value()));
                           });
    }

    ENDPOINT_INFO(saveMusicCandidate) {
        info->summary = "Save a music candidate to the library as a new piece or a new version of a piece";
        info->description = "Copies the candidate's WAV under music/ with its provenance and records the version. "
                            "Idempotent for a candidate that is already a version of the piece.";
        info->addTag("Music Library");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_201, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/music/generated/{generationId}/save", saveMusicCandidate, PATH(String, generationId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/music/generated/{generationId}/save", "POST",
                           "api/v1/music/generated/{generationId}/save", "saveMusicCandidate", "MusicController",
                           request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               const std::string id = generationId ? std::string(*generationId) : std::string{};
                               if (!isUuidShape(id))
                                   return bailHttp(span, Status::CODE_400, "music generation id must be a UUID");
                               if (span)
                                   span->setAttribute("music.generation_id", id);
                               std::shared_ptr<OutgoingResponse> failure;
                               auto parsed = parseBody(request, "music.save", "music save request",
                                                       api::musicSaveRequestFromJson, span, failure);
                               if (!parsed)
                                   return failure;
                               bool created = false;
                               auto saved = musicService_.save(id, *parsed, created, span);
                               if (!saved.isSuccess())
                                   return bailFromServerError(span, saved.getError().value());
                               return pieceResponse(span, created ? Status::CODE_201 : Status::CODE_200,
                                                    saved.getValue().value());
                           });
    }

    ENDPOINT_INFO(refineMusicPiece) {
        info->summary = "Ask the AI to refine a piece: an instruction in, proposed sections and a diff out";
        info->description =
            "Synchronous and generates no audio. Sends the version's editable sections and the instruction to "
            "ElevenLabs' planner and reports which sections it changed, so the console can show the proposal and "
            "then generate in sections mode with `keep` = `kept`.";
        info->addTag("Music Library");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/music/{pieceId}/refine", refineMusicPiece, PATH(String, pieceId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/music/{pieceId}/refine", "POST", "api/v1/music/{pieceId}/refine", "refineMusicPiece",
            "MusicController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                const std::string id = pieceId ? std::string(*pieceId) : std::string{};
                if (!isUuidShape(id))
                    return bailHttp(span, Status::CODE_400, "music piece id must be a UUID");
                if (span)
                    span->setAttribute("music.piece_id", id);
                std::shared_ptr<OutgoingResponse> failure;
                auto parsed = parseBody(request, "music.refine", "music refine request",
                                        api::musicRefineRequestFromJson, span, failure);
                if (!parsed)
                    return failure;
                auto refined = musicService_.refine(id, *parsed, span);
                if (!refined.isSuccess())
                    return bailFromServerError(span, refined.getError().value());
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, api::musicRefineResultToJson(refined.getValue().value()));
            });
    }

    ENDPOINT_INFO(planMusic) {
        info->summary = "Draft editable sections for a new piece from a prompt and a length";
        info->addTag("Music Library");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/music/plan", planMusic, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/music/plan", "POST", "api/v1/music/plan", "planMusic", "MusicController",
                           request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               std::shared_ptr<OutgoingResponse> failure;
                               auto parsed = parseBody(request, "music.plan", "music plan request",
                                                       api::musicPlanRequestFromJson, span, failure);
                               if (!parsed)
                                   return failure;
                               auto planned = musicService_.plan(*parsed, span);
                               if (!planned.isSuccess())
                                   return bailFromServerError(span, planned.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(span, Status::CODE_200,
                                                   api::musicPlanResultToJson(planned.getValue().value()));
                           });
    }

    ENDPOINT_INFO(listMusicPieces) {
        info->summary = "List the music library, newest first";
        info->addTag("Music Library");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/music", listMusicPieces, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/music", "GET", "api/v1/music", "listMusicPieces", "MusicController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                auto pieces = musicService_.list(span);
                if (!pieces.isSuccess())
                    return bailFromServerError(span, pieces.getError().value());
                if (span) {
                    span->setAttribute("music.piece_count", static_cast<int64_t>(pieces.getValue().value().size()));
                    span->setHttpStatus(200);
                }
                return jsonResponse(span, Status::CODE_200,
                                    api::listResponseToJson(pieces.getValue().value(), musicPieceToJson));
            });
    }

    ENDPOINT_INFO(getMusicPiece) {
        info->summary = "Get one music piece with all its versions";
        info->addTag("Music Library");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/music/{pieceId}", getMusicPiece, PATH(String, pieceId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/music/{pieceId}", "GET", "api/v1/music/{pieceId}", "getMusicPiece",
                           "MusicController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               const std::string id = pieceId ? std::string(*pieceId) : std::string{};
                               if (!isUuidShape(id))
                                   return bailHttp(span, Status::CODE_400, "music piece id must be a UUID");
                               auto piece = musicService_.get(id, span);
                               if (!piece.isSuccess())
                                   return bailFromServerError(span, piece.getError().value());
                               return pieceResponse(span, Status::CODE_200, piece.getValue().value());
                           });
    }

    ENDPOINT_INFO(updateMusicPiece) {
        info->summary = "Rename a piece, edit its notes, or choose which version is current";
        info->addTag("Music Library");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("PUT", "api/v1/music/{pieceId}", updateMusicPiece, PATH(String, pieceId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("PUT /api/v1/music/{pieceId}", "PUT", "api/v1/music/{pieceId}", "updateMusicPiece",
                           "MusicController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               const std::string id = pieceId ? std::string(*pieceId) : std::string{};
                               if (!isUuidShape(id))
                                   return bailHttp(span, Status::CODE_400, "music piece id must be a UUID");
                               std::shared_ptr<OutgoingResponse> failure;
                               auto parsed = parseBody(request, "music.piece.update", "music piece update request",
                                                       api::musicPieceUpdateRequestFromJson, span, failure);
                               if (!parsed)
                                   return failure;
                               auto updated = musicService_.update(id, *parsed, span);
                               if (!updated.isSuccess())
                                   return bailFromServerError(span, updated.getError().value());
                               return pieceResponse(span, Status::CODE_200, updated.getValue().value());
                           });
    }

    ENDPOINT_INFO(deleteMusicPiece) {
        info->summary = "Delete a piece from the library (its WAVs are retained)";
        info->addTag("Music Library");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("DELETE", "api/v1/music/{pieceId}", deleteMusicPiece, PATH(String, pieceId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("DELETE /api/v1/music/{pieceId}", "DELETE", "api/v1/music/{pieceId}", "deleteMusicPiece",
                           "MusicController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               const std::string id = pieceId ? std::string(*pieceId) : std::string{};
                               if (!isUuidShape(id))
                                   return bailHttp(span, Status::CODE_400, "music piece id must be a UUID");
                               if (span)
                                   span->setAttribute("music.piece_id", id);
                               auto removed = musicService_.remove(id, span);
                               if (!removed.isSuccess())
                                   return bailFromServerError(span, removed.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(
                                   span, Status::CODE_200,
                                   api::statusResponseToJson(api::StatusResponse{
                                       "ok", 200, fmt::format("Music piece {} deleted", id), std::nullopt}));
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
