#include "server/transport/MusicHandlers.h"

#include <cstdint>
#include <memory>
#include <string>

#include <fmt/format.h>

#include "api/DialogContracts.h"
#include "api/JobResponses.h"
#include "api/JsonResponse.h"
#include "api/MusicContracts.h"
#include "model/MusicPiece.h"
#include "server/jobs/JobWorker.h"
#include "server/transport/HandlerSupport.h"
#include "server/ws/service/DialogMusicService.h"
#include "server/ws/service/MusicService.h"
#include "util/ObservabilityManager.h"
#include "util/UuidValidation.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<jobs::JobWorker> jobWorker;
}

namespace creatures::transport {
namespace {

PreparedResponse pieceResponse(const int statusCode, const MusicPiece &piece,
                               const std::shared_ptr<OperationSpan> &span) {
    if (span) {
        span->setAttribute("music.piece_id", piece.id);
        span->setAttribute("music.version_count", static_cast<int64_t>(piece.versions.size()));
        span->setSuccess();
    }
    return PreparedResponse::json(statusCode, api::jsonToString(musicPieceToJson(piece)));
}

/// Both music job kinds share the same two-slot queue and the same admission
/// replies; only the job type and the words differ.
PreparedResponse queueMusicJob(const std::string &details, const jobs::JobType type, const char *jobKind,
                               const char *fullMessage, const char *enqueueFailedMessage,
                               const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::jobWorker)
        return errorStatus(500, "Music jobs unavailable: job worker missing", span, "MissingDependencies");
    const auto admission = creatures::jobWorker->tryCreateAndQueueMusicJob(details, span, type);
    if (admission.status == jobs::JobWorker::QueueAdmission::Status::Full)
        return errorStatus(429, fullMessage, span, "QueueAdmissionRejected");
    if (admission.status == jobs::JobWorker::QueueAdmission::Status::EnqueueFailed)
        return errorStatus(500, enqueueFailedMessage, span, "QueueEnqueueFailure");
    if (span) {
        span->setAttribute("job.id", admission.jobId);
        span->setSuccess();
    }
    const api::JobCreatedResponse response{
        admission.jobId, jobKind,
        fmt::format("{} job created; listen for job-progress and job-complete messages.",
                    type == jobs::JobType::Music ? "Music" : "Dialog music")};
    return PreparedResponse::json(202, api::jsonToString(api::jobCreatedResponseToJson(response)));
}

constexpr const char *MUSIC_QUEUE_FULL = "Two music generations are already queued or running; try again shortly";

} // namespace

PreparedResponse generateMusic(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed =
        parseBody(body, "music.generate", "music generate request", api::musicGenerateRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    if (span) {
        span->setAttribute("music.request_kind", request.requestKind());
        span->setAttribute("music.model_id", request.modelId);
        if (!request.pieceId.empty())
            span->setAttribute("music.piece_id", request.pieceId);
        if (request.sections) {
            span->setAttribute("music.section_count", static_cast<int64_t>(request.sections->size()));
            span->setAttribute("music.kept_count", static_cast<int64_t>(request.keep.size()));
        }
    }
    return queueMusicJob(api::musicGenerateRequestToJson(request).dump(), jobs::JobType::Music, "music",
                         MUSIC_QUEUE_FULL, "Could not queue music job", span);
}

PreparedResponse planMusic(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed = parseBody(body, "music.plan", "music plan request", api::musicPlanRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const ws::MusicService service;
    const auto planned = service.plan(parsed.getValue().value(), span);
    if (!planned.isSuccess())
        return serverErrorStatus(planned.getError().value(), span);
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, api::jsonToString(api::musicPlanResultToJson(planned.getValue().value())));
}

PreparedResponse listMusicPieces(const std::shared_ptr<OperationSpan> &span) {
    const ws::MusicService service;
    const auto pieces = service.list(span);
    if (!pieces.isSuccess())
        return serverErrorStatus(pieces.getError().value(), span);
    if (span) {
        span->setAttribute("music.piece_count", static_cast<int64_t>(pieces.getValue().value().size()));
        span->setSuccess();
    }
    return PreparedResponse::json(
        200, api::jsonToString(api::listResponseToJson(pieces.getValue().value(), musicPieceToJson)));
}

PreparedResponse getMusicPiece(const std::string &pieceId, const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(pieceId))
        return errorStatus(400, "music piece id must be a UUID", span, "InvalidPieceId");
    const ws::MusicService service;
    const auto piece = service.get(pieceId, span);
    if (!piece.isSuccess())
        return serverErrorStatus(piece.getError().value(), span);
    return pieceResponse(200, piece.getValue().value(), span);
}

PreparedResponse updateMusicPiece(const std::string &pieceId, const std::string &body,
                                  const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(pieceId))
        return errorStatus(400, "music piece id must be a UUID", span, "InvalidPieceId");
    const auto parsed =
        parseBody(body, "music.piece.update", "music piece update request", api::musicPieceUpdateRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const ws::MusicService service;
    const auto updated = service.update(pieceId, parsed.getValue().value(), span);
    if (!updated.isSuccess())
        return serverErrorStatus(updated.getError().value(), span);
    return pieceResponse(200, updated.getValue().value(), span);
}

PreparedResponse deleteMusicPiece(const std::string &pieceId, const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(pieceId))
        return errorStatus(400, "music piece id must be a UUID", span, "InvalidPieceId");
    if (span)
        span->setAttribute("music.piece_id", pieceId);
    const ws::MusicService service;
    const auto removed = service.remove(pieceId, span);
    if (!removed.isSuccess())
        return serverErrorStatus(removed.getError().value(), span);
    return okStatus(200, fmt::format("Music piece {} deleted", pieceId), span);
}

PreparedResponse refineMusicPiece(const std::string &pieceId, const std::string &body,
                                  const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(pieceId))
        return errorStatus(400, "music piece id must be a UUID", span, "InvalidPieceId");
    if (span)
        span->setAttribute("music.piece_id", pieceId);
    const auto parsed = parseBody(body, "music.refine", "music refine request", api::musicRefineRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const ws::MusicService service;
    const auto refined = service.refine(pieceId, parsed.getValue().value(), span);
    if (!refined.isSuccess())
        return serverErrorStatus(refined.getError().value(), span);
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, api::jsonToString(api::musicRefineResultToJson(refined.getValue().value())));
}

PreparedResponse saveMusicCandidate(const std::string &generationId, const std::string &body,
                                    const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(generationId))
        return errorStatus(400, "music generation id must be a UUID", span, "InvalidGenerationId");
    if (span)
        span->setAttribute("music.generation_id", generationId);
    const auto parsed = parseBody(body, "music.save", "music save request", api::musicSaveRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const ws::MusicService service;
    bool created = false;
    const auto saved = service.save(generationId, parsed.getValue().value(), created, span);
    if (!saved.isSuccess())
        return serverErrorStatus(saved.getError().value(), span);
    return pieceResponse(created ? 201 : 200, saved.getValue().value(), span);
}

PreparedResponse getMusicCandidateRecipe(const std::string &generationId, const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(generationId))
        return errorStatus(400, "music generation id must be a UUID", span, "InvalidGenerationId");
    if (span)
        span->setAttribute("music.generation_id", generationId);
    const ws::DialogMusicService service;
    const auto recipe = service.recipe(generationId, span);
    if (!recipe.isSuccess())
        return serverErrorStatus(recipe.getError().value(), span);
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, api::jsonToString(api::dialogMusicRecipeToJson(recipe.getValue().value())));
}

PreparedResponse submitDialogMusic(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed =
        parseBody(body, "dialog.music.submit", "dialog music request", api::dialogMusicRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    if (span) {
        span->setAttribute("dialog.script_id", canonicalUuid(request.scriptId));
        span->setAttribute("dialog.generation_id", canonicalUuid(request.dialogGenerationId));
        span->setAttribute("dialog.cache_key", request.dialogCacheKey);
        span->setAttribute("music.model_id", request.modelId);
        span->setAttribute("music.request_kind", request.isPlanMode() ? "composition_plan" : "prompt");
        span->setAttribute("music.store_for_inpainting", request.storeForInpainting);
        span->setAttribute("music.finetune_present", request.finetuneId.has_value());
        if (request.isPlanMode()) {
            span->setAttribute("music.chunk_count", static_cast<int64_t>(request.compositionPlan->chunks.size()));
            span->setAttribute("music.seed_present", request.seed.has_value());
        } else {
            span->setAttribute("music.generation_mode", request.generationMode);
            span->setAttribute("music.prompt_length", static_cast<int64_t>(request.prompt.size()));
            span->setAttribute("music.duration_extension_ms", request.durationExtensionMs);
            span->setAttribute("music.force_instrumental", request.forceInstrumental);
        }
    }
    return queueMusicJob(api::dialogMusicRequestToJson(request).dump(), jobs::JobType::DialogMusic, "dialog-music",
                         MUSIC_QUEUE_FULL, "Could not queue dialog music job", span);
}

PreparedResponse planDialogMusic(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed =
        parseBody(body, "dialog.music.plan", "dialog music plan request", api::dialogMusicPlanRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    if (span) {
        span->setAttribute("dialog.generation_id", canonicalUuid(request.dialogGenerationId));
        span->setAttribute("dialog.cache_key", request.dialogCacheKey);
        span->setAttribute("music.model_id", request.modelId);
        span->setAttribute("music.prompt_length", static_cast<int64_t>(request.prompt.size()));
    }
    const ws::DialogMusicService service;
    const auto planned = service.plan(request, span);
    if (!planned.isSuccess())
        return serverErrorStatus(planned.getError().value(), span);
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, api::jsonToString(api::dialogMusicPlanResultToJson(planned.getValue().value())));
}

PreparedResponse listMusicFinetunes(const std::shared_ptr<OperationSpan> &span) {
    const ws::DialogMusicService service;
    const auto finetunes = service.listFinetunes(span);
    if (!finetunes.isSuccess())
        return serverErrorStatus(finetunes.getError().value(), span);
    if (span) {
        span->setAttribute("music.finetune_count", static_cast<int64_t>(finetunes.getValue().value().size()));
        span->setSuccess();
    }
    return PreparedResponse::json(
        200, api::jsonToString(api::listResponseToJson(finetunes.getValue().value(), api::musicFinetuneToJson)));
}

PreparedResponse promoteGeneratedMusic(const std::string &generationId, const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(generationId))
        return errorStatus(400, "music generation id must be a UUID", span, "InvalidGenerationId");
    if (span)
        span->setAttribute("music.generation_id", generationId);
    const ws::DialogMusicService service;
    const auto promoted = service.promote(generationId, span);
    if (!promoted.isSuccess())
        return serverErrorStatus(promoted.getError().value(), span);
    if (span)
        span->setSuccess();
    return PreparedResponse::json(
        200, api::jsonToString(api::dialogMusicPromotionResultToJson(promoted.getValue().value())));
}

} // namespace creatures::transport
