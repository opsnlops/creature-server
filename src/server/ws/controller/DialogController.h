#pragma once

#include <memory>

#include <fmt/format.h>

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "api/DialogContracts.h"
#include "api/JobResponses.h"
#include "model/DialogScript.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobState.h"
#include "server/jobs/JobWorker.h"
#include "server/namespace-stuffs.h"
#include "server/voice/ScriptCacheKey.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/dto/StatusDto.h"
#include "util/JsonParser.h"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures {
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
} // namespace creatures

namespace creatures::ws {

/// HTTP surface for the multi-character dialog generator (Phases 1–5).
///
/// One POST submits a scene; the server validates the request shape, queues a
/// job in the shared JobWorker, and returns 202 with the job_id. The actual
/// pipeline runs on the worker thread; clients receive progress + completion
/// updates over the existing WebSocket job-progress stream. There is no GET
/// status endpoint — the job system is push-based.
class DialogController : public oatpp::web::server::api::ApiController, public HttpResponseHelpers<DialogController> {
  public:
    DialogController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) : ApiController(objectMapper) {}

    static std::shared_ptr<DialogController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                          objectMapper)) {
        return std::make_shared<DialogController>(objectMapper);
    }

    ENDPOINT_INFO(submitDialog) {
        info->summary = "Submit a multi-character dialog scene for assembly (async job)";
        info->description =
            "Generates a multi-character dialog scene end-to-end: ElevenLabs Text-to-Dialogue + forced alignment + "
            "per-creature slicing + 17-channel WAV + multi-track Animation. Returns 202 with a job_id; the worker "
            "runs the rest asynchronously and publishes progress + completion over the WebSocket job-progress "
            "stream. Filter for the returned job_id to follow this scene's job.";
        info->addTag("Multi-character Dialog");
        info->addResponse<oatpp::String>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog", submitDialog, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog", "POST", "api/v1/animation/dialog", "submitDialog", "DialogController",
            request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                const auto body = readRequestBodyLimited(request, api::MAX_DIALOG_REQUEST_BYTES, span);
                const auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                                      "DialogController.parseDialogRequest", span)
                                                                : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "dialog.submit");
                const auto json = JsonParser::parseApiJsonString(body, "dialog request", parseSpan);
                if (!json.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, json.getError().value());
                }
                const auto parsed = api::dialogRequestFromJson(json.getValue().value());
                if (!parsed.isSuccess()) {
                    const auto error = parsed.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidDialogRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                const auto dialogRequest = parsed.getValue().value();
                const bool hasTurns = !dialogRequest.turns.empty();
                const bool hasScriptId = dialogRequest.scriptId.has_value();

                // ---- Accepted voice take gate (#131) -----------------------
                // Strict, not fallback-soft: un-auditioned audio must never
                // reach the birds. Enforced HERE rather than in the worker so
                // the caller gets a synchronous 400 instead of a job that
                // accepts, queues, and then fails.
                //
                // Only script renders are gated. Inline `turns` have no script
                // to carry an acceptance, so they keep working as they always
                // have. An explicit generation_id is still an override, for
                // the CLI and tooling.
                const bool hasExplicitGeneration = dialogRequest.generationId.has_value();
                if (hasScriptId && !hasExplicitGeneration) {
                    auto gateSpan =
                        creatures::observability->createChildOperationSpan("DialogController.acceptedVoiceGate", span);
                    auto scriptResult = creatures::db->getDialogScript(*dialogRequest.scriptId, gateSpan);
                    if (!scriptResult.isSuccess()) {
                        return bailFromServerError(span, scriptResult.getError().value());
                    }
                    const auto script = scriptResult.getValue().value();

                    if (!script.accepted_voice) {
                        return bailHttp(span, Status::CODE_400,
                                        "no accepted voice take — audition and accept one first");
                    }
                    const auto fresh = creatures::voice::acceptedVoiceIsFresh(script, gateSpan);
                    if (!fresh.has_value()) {
                        return bailHttp(span, Status::CODE_400,
                                        "could not check the accepted voice take against the script's turns — a "
                                        "creature is missing or has no voice configured");
                    }
                    if (!*fresh) {
                        return bailHttp(span, Status::CODE_400,
                                        "the accepted voice take predates the current turns — re-audition and "
                                        "accept");
                    }
                    if (span) {
                        span->setAttribute("dialog.accepted_generation_id", script.accepted_voice->generation_id);
                    }
                }
                const auto detailsStr = api::dialogRequestToJson(dialogRequest).dump();

                if (span) {
                    if (hasTurns) {
                        span->setAttribute("dialog.turns", static_cast<int64_t>(dialogRequest.turns.size()));
                    }
                    if (hasScriptId) {
                        span->setAttribute("dialog.script_id", canonicalUuid(*dialogRequest.scriptId));
                    }
                    span->setAttribute("dialog.persistence", dialogRequest.persistence);
                    span->setAttribute("dialog.autoplay", dialogRequest.autoplay);
                }

                const auto admission =
                    creatures::jobWorker->tryCreateAndQueueJob(creatures::jobs::JobType::Dialog, detailsStr, span);
                if (admission.status == creatures::jobs::JobWorker::QueueAdmission::Status::Full) {
                    return bailHttp(span, Status::CODE_429,
                                    "Eight dialog jobs are already queued or running; try again shortly", nullptr,
                                    "QueueAdmissionRejected");
                }
                if (admission.status == creatures::jobs::JobWorker::QueueAdmission::Status::EnqueueFailed) {
                    return bailHttp(span, Status::CODE_500, "Could not queue dialog job", nullptr,
                                    "QueueEnqueueFailure");
                }
                const auto &jobId = admission.jobId;

                if (span) {
                    span->setAttribute("job.id", jobId);
                    span->setHttpStatus(202);
                }

                api::JobCreatedResponse response;
                response.jobId = jobId;
                response.jobType = "dialog";
                response.message =
                    hasScriptId
                        ? fmt::format("Dialog job created from script {}. Listen for job-progress and job-complete "
                                      "WebSocket messages on this job_id.",
                                      *dialogRequest.scriptId)
                        : fmt::format("Dialog job created with {} turn(s). Listen for job-progress and "
                                      "job-complete WebSocket messages on this job_id.",
                                      dialogRequest.turns.size());
                return jsonResponse(span, Status::CODE_202, api::jobCreatedResponseToJson(response));
            });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
