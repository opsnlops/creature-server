#include "server/transport/DialogHandlers.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "api/DialogContracts.h"
#include "api/JobResponses.h"
#include "api/JsonResponse.h"
#include "model/DialogScript.h"
#include "server/database.h"
#include "server/jobs/JobWorker.h"
#include "server/script/DialogScriptMutationLock.h"
#include "server/storage/Storage.h"
#include "server/transport/HandlerSupport.h"
#include "server/voice/DialogCache.h"
#include "server/voice/ScriptCacheKey.h"
#include "server/ws/service/DialogPreviewService.h"
#include "util/ObservabilityManager.h"
#include "util/Slugify.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
} // namespace creatures

namespace creatures::transport {
namespace {

constexpr const char *DIALOG_QUEUE_FULL = "Eight dialog jobs are already queued or running; try again shortly";

int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// All dialog-family jobs share the general eight-slot queue.
PreparedResponse queueDialogJob(const jobs::JobType type, const std::string &details, const char *enqueueFailedMessage,
                                api::JobCreatedResponse response, const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::jobWorker)
        return errorStatus(500, "Dialog jobs unavailable: job worker missing", span, "MissingDependencies");
    const auto admission = creatures::jobWorker->tryCreateAndQueueJob(type, details, span);
    if (admission.status == jobs::JobWorker::QueueAdmission::Status::Full)
        return errorStatus(429, DIALOG_QUEUE_FULL, span, "QueueAdmissionRejected");
    if (admission.status == jobs::JobWorker::QueueAdmission::Status::EnqueueFailed)
        return errorStatus(500, enqueueFailedMessage, span, "QueueEnqueueFailure");
    response.jobId = admission.jobId;
    if (span) {
        span->setAttribute("job.id", admission.jobId);
        span->setSuccess();
    }
    return PreparedResponse::json(202, api::jsonToString(api::jobCreatedResponseToJson(response)));
}

std::string iso8601(const std::chrono::system_clock::time_point &when) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
    const auto asTime = static_cast<std::time_t>(seconds);
    std::tm parts{};
    gmtime_r(&asTime, &parts);
    return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                       parts.tm_hour, parts.tm_min, parts.tm_sec);
}

} // namespace

PreparedResponse submitDialog(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::db)
        return errorStatus(500, "Dialog submission unavailable: database missing", span, "MissingDependencies");
    const auto parsed = parseBody(body, "dialog.submit", "dialog request", api::dialogRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    const bool hasTurns = !request.turns.empty();
    const bool hasScriptId = request.scriptId.has_value();

    // Accepted voice take gate (#131). Strict, not fallback-soft: un-auditioned
    // audio must never reach the birds. Enforced here rather than in the worker
    // so the caller gets a synchronous 400 instead of a job that accepts,
    // queues, and then fails. Only script renders are gated; inline turns have
    // no script to carry an acceptance, and an explicit generation_id is an
    // override for the CLI and tooling.
    if (hasScriptId && !request.generationId.has_value()) {
        auto gateSpan = childSpan("DialogController.acceptedVoiceGate", span);
        const auto scriptResult = creatures::db->getDialogScript(*request.scriptId, gateSpan);
        if (!scriptResult.isSuccess())
            return serverErrorStatus(scriptResult.getError().value(), span);
        const auto script = scriptResult.getValue().value();
        if (!script.accepted_voice)
            return errorStatus(400, "no accepted voice take — audition and accept one first", span, "NoAcceptedVoice");
        const auto fresh = creatures::voice::acceptedVoiceIsFresh(script, gateSpan);
        if (!fresh.has_value()) {
            return errorStatus(400,
                               "could not check the accepted voice take against the script's turns — a creature "
                               "is missing or has no voice configured",
                               span, "AcceptedVoiceUncheckable");
        }
        if (!*fresh) {
            return errorStatus(400, "the accepted voice take predates the current turns — re-audition and accept", span,
                               "AcceptedVoiceStale");
        }
        if (span)
            span->setAttribute("dialog.accepted_generation_id", script.accepted_voice->generation_id);
    }
    if (span) {
        if (hasTurns)
            span->setAttribute("dialog.turns", static_cast<int64_t>(request.turns.size()));
        if (hasScriptId)
            span->setAttribute("dialog.script_id", canonicalUuid(*request.scriptId));
        span->setAttribute("dialog.persistence", request.persistence);
        span->setAttribute("dialog.autoplay", request.autoplay);
    }
    api::JobCreatedResponse response;
    response.jobType = "dialog";
    response.message = hasScriptId ? fmt::format("Dialog job created from script {}. Listen for job-progress and "
                                                 "job-complete WebSocket messages on this job_id.",
                                                 *request.scriptId)
                                   : fmt::format("Dialog job created with {} turn(s). Listen for job-progress and "
                                                 "job-complete WebSocket messages on this job_id.",
                                                 request.turns.size());
    return queueDialogJob(jobs::JobType::Dialog, api::dialogRequestToJson(request).dump(), "Could not queue dialog job",
                          std::move(response), span);
}

PreparedResponse acceptVoiceTake(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::db)
        return errorStatus(500, "Voice acceptance unavailable: database missing", span, "MissingDependencies");
    const auto parsed =
        parseBody(body, "dialog.voice.accept", "accept voice take request", api::acceptVoiceTakeRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    const auto &scriptId = request.scriptId;
    const auto &generationId = request.generationId;
    const auto &cacheKey = request.dialogCacheKey;
    if (span) {
        span->setAttribute("script.id", canonicalUuid(scriptId));
        span->setAttribute("dialog.generation_id", canonicalUuid(generationId));
        span->setAttribute("dialog.cache_key", cacheKey);
    }
    auto opSpan = childSpan("DialogVoiceController.acceptVoiceTake", span);
    const std::scoped_lock mutationLock(script::mutationMutex());
    const auto existing = creatures::db->getDialogScript(scriptId, opSpan);
    if (!existing.isSuccess())
        return serverErrorStatus(existing.getError().value(), span);
    auto script = existing.getValue().value();

    // The take must have been auditioned against the turns as they stand.
    // Accepting against edited turns would store something that reads as
    // stale the instant it lands, which is worse than refusing.
    const auto currentKey = creatures::voice::computeScriptCacheKey(script.turns, opSpan);
    if (!currentKey.isSuccess())
        return serverErrorStatus(currentKey.getError().value(), span);
    if (currentKey.getValue().value() != cacheKey) {
        return errorStatus(400,
                           "dialog_cache_key does not match the script's current turns — the script was edited "
                           "after this take was auditioned. Re-audition and accept again.",
                           span, "StaleCacheKey");
    }
    if (script.accepted_voice && script.accepted_voice->generation_id == generationId) {
        // Already accepted: re-promoting would look for an ad-hoc file that
        // has already moved.
        if (span)
            span->setSuccess();
        return PreparedResponse::json(200, api::jsonToString(dialogScriptToJson(script)));
    }
    if (script.updated_at == std::numeric_limits<int64_t>::max())
        return errorStatus(400, "dialog script updated_at cannot advance", span, "UpdatedAtSaturated");

    // Promotion moves the take's 17-channel WAV out of the ad-hoc bucket, so
    // there has to be one. Generation writes it now (#131), which makes this
    // the ordinary case: a plain, fast 200 with the canonical script.
    const auto adHocPath = storage::voiceTakeAdHocPath(generationId);
    if (!adHocPath.isSuccess())
        return serverErrorStatus(adHocPath.getError().value(), span);
    std::error_code ec;
    const std::filesystem::path takeAudio = adHocPath.getValue().value();
    const bool audioReady =
        std::filesystem::exists(takeAudio, ec) && !ec && std::filesystem::file_size(takeAudio, ec) > 0;
    if (span)
        span->setAttribute("voice.take_audio_ready", audioReady);
    if (!audioReady) {
        // A take from before generation wrote its export, or one whose file
        // the ad-hoc sweep took while its generation survived. It can be
        // rebuilt from the cached generation without ElevenLabs, but a long
        // scene is hundreds of MB of assembly: a job, not a request. Check the
        // take exists first so an unknown id is still a synchronous 404.
        const auto takes = creatures::voice::listGenerations(cacheKey);
        const bool known = std::any_of(takes.begin(), takes.end(),
                                       [&](const auto &entry) { return entry.generationId == generationId; });
        if (!known) {
            return errorStatus(
                404,
                fmt::format("generation '{}' not found for this script's turns (expired or never existed)",
                            generationId),
                span, "NotFound");
        }
        nlohmann::json details;
        details["script_id"] = scriptId;
        details["generation_id"] = generationId;
        details["dialog_cache_key"] = cacheKey;
        api::JobCreatedResponse response;
        response.jobType = "voice-take-accept";
        response.message = "This take's audio has to be assembled before it can be accepted. Listen for "
                           "job-progress and job-complete WebSocket messages on this job_id, or poll GET "
                           "/api/v1/job/{job_id}; the completion result is the updated script.";
        return queueDialogJob(jobs::JobType::VoiceTakeAccept, details.dump(), "Could not queue voice acceptance job",
                              std::move(response), span);
    }

    // Acceptance promises that this exact performance survives cache sweeps
    // and reboots. Make the cached generation durable before changing either
    // the WAV location or the script.
    const bool durableAlreadyExisted = creatures::voice::acceptedGenerationExists(cacheKey, generationId);
    if (!durableAlreadyExisted) {
        const auto madeDurable = creatures::voice::saveAcceptedGeneration(cacheKey, generationId);
        if (!madeDurable.isSuccess()) {
            static_cast<void>(creatures::voice::removeAcceptedGeneration(cacheKey, generationId));
            return serverErrorStatus(madeDurable.getError().value(), span);
        }
    }
    const auto discardNewDurableCopy = [&] {
        if (!durableAlreadyExisted)
            static_cast<void>(creatures::voice::removeAcceptedGeneration(cacheKey, generationId));
    };
    const auto previousAcceptance = script.accepted_voice;
    const auto filename = util::exportBasename(script.title, generationId) + ".wav";
    const auto promoted = storage::promoteVoiceTake(generationId, filename, opSpan);
    if (!promoted.isSuccess()) {
        discardNewDurableCopy();
        return serverErrorStatus(promoted.getError().value(), span);
    }
    AcceptedVoice accepted;
    accepted.generation_id = generationId;
    accepted.dialog_cache_key = cacheKey;
    accepted.sound_file = promoted.getValue().value().forMetadata;
    accepted.accepted_at = nowMillis();
    script.accepted_voice = accepted;
    auto updated = dialogScriptToJson(script);
    updated["updated_at"] = std::max(nowMillis(), script.updated_at + 1);
    const auto published = storage::publishDialogScript(updated.dump(), opSpan);
    if (!published.isSuccess()) {
        // Publication is the commit point. Roll the new WAV back to ad-hoc and
        // remove a newly-created durable generation; the old acceptance has
        // not been touched yet.
        const auto rollback = storage::demoteVoiceTake(accepted.sound_file, generationId, opSpan);
        if (!rollback.isSuccess()) {
            spdlog::warn("could not roll back voice take {} after script publish failed: {}", generationId,
                         rollback.getError()->getMessage());
        }
        discardNewDurableCopy();
        return serverErrorStatus(published.getError().value(), span);
    }
    // The script now points at the new take, so cleanup of the old assets is
    // best effort. A cleanup failure may waste disk, but cannot leave MongoDB
    // pointing at a moved file.
    if (previousAcceptance) {
        const auto demoted =
            storage::demoteVoiceTake(previousAcceptance->sound_file, previousAcceptance->generation_id, opSpan);
        if (!demoted.isSuccess()) {
            spdlog::warn("accepted voice take {} but could not demote previous take {}: {}", generationId,
                         previousAcceptance->generation_id, demoted.getError()->getMessage());
        }
        static_cast<void>(creatures::voice::removeAcceptedGeneration(previousAcceptance->dialog_cache_key,
                                                                     previousAcceptance->generation_id));
    }
    spdlog::info("accepted voice take {} for script '{}' -> {}", generationId, script.title, accepted.sound_file);
    if (span) {
        span->setAttribute("voice.sound_file", accepted.sound_file);
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(dialogScriptToJson(published.getValue().value())));
}

PreparedResponse lookupDialogPreview(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed = parseBody(body, "dialog.preview.lookup", "dialog preview lookup request",
                                  api::dialogPreviewLookupRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto turns = parsed.getValue().value();
    auto opSpan = childSpan("DialogPreviewController.lookupPreview", span);
    const auto resolved = ws::DialogPreviewService::resolveCreatures(turns, opSpan);
    if (!resolved.isSuccess())
        return serverErrorStatus(resolved.getError().value(), span);
    const auto inputs = ws::DialogPreviewService::buildDialogInputs(turns, resolved.getValue().value());
    const auto cacheKey = creatures::voice::computeCacheKey(inputs);
    const auto generations = creatures::voice::listGenerations(cacheKey);
    // An empty cache is a fact about the cache, not an error (#204): the key is
    // deterministic from the turns and is what the console uses to judge
    // whether the script's accepted voice is still fresh.
    api::DialogPreviewLookupResponse response;
    response.cacheKey = cacheKey;
    if (!generations.empty())
        response.latestGenerationId = generations.front().generationId;
    response.generations.reserve(generations.size());
    for (const auto &generation : generations)
        response.generations.push_back({generation.generationId, iso8601(generation.createdAt)});
    if (span) {
        span->setAttribute("dialog.cache_key", cacheKey);
        span->setAttribute("dialog.generations", static_cast<int64_t>(generations.size()));
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(api::dialogPreviewLookupResponseToJson(response)));
}

PreparedResponse submitDialogPreviewMeta(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed =
        parseBody(body, "dialog.preview", "dialog preview request", api::dialogPreviewRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    auto opSpan = childSpan("DialogPreviewController.submitPreviewMeta", span);
    // Fast path: a specific generation_id, or the latest take when regenerate
    // is false, is a cheap disk read served synchronously. Anything requiring
    // ElevenLabs becomes a job.
    ws::DialogPreviewService service;
    const auto fastResult = service.tryServeFromCache(request, opSpan, "meta");
    if (!fastResult.isSuccess())
        return serverErrorStatus(fastResult.getError().value(), span);
    const auto fast = fastResult.getValue().value();
    if (fast.cacheHit) {
        const auto response = ws::DialogPreviewService::makeMetaResponse(fast.outcome->generation,
                                                                         fast.outcome->cacheKey, fast.outcome->cached);
        if (span)
            span->setSuccess();
        return PreparedResponse::json(200, api::jsonToString(api::dialogPreviewMetaResponseToJson(response)));
    }
    api::JobCreatedResponse response;
    response.jobType = "dialog-preview";
    response.message = "Dialog preview job created. Listen for job-progress and job-complete WebSocket messages on "
                       "this job_id, or poll GET /api/v1/job/{job_id}.";
    return queueDialogJob(jobs::JobType::DialogPreview, api::dialogPreviewRequestToJson(request).dump(),
                          "Could not queue dialog preview job", std::move(response), span);
}

PreparedResponse submitDialogPreviewMultichannel(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed =
        parseBody(body, "dialog.preview", "dialog preview request", api::dialogPreviewRequestFromJson, span);
    if (!parsed.isSuccess())
        return serverErrorStatus(parsed.getError().value(), span);
    // Always a job. Even a fully-cached long scene means writing a ~0.5 GB
    // 17-channel WAV, which must never ride one HTTP response.
    api::JobCreatedResponse response;
    response.jobType = "dialog-preview-export";
    response.message = "Dialog preview export job created. The 17-channel WAV lands in the ad-hoc sound bucket; "
                       "the completion result carries its file_name (downloadable via GET "
                       "/api/v1/sound/ad-hoc/{filename}). Listen for job-complete on this job_id, or poll GET "
                       "/api/v1/job/{job_id}.";
    return queueDialogJob(jobs::JobType::DialogPreviewExport,
                          api::dialogPreviewRequestToJson(parsed.getValue().value()).dump(),
                          "Could not queue dialog preview export job", std::move(response), span);
}

} // namespace creatures::transport
