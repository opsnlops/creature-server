#pragma once

#include <memory>

#include <fmt/format.h>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "server/jobs/JobManager.h"
#include "server/jobs/JobState.h"
#include "server/jobs/JobWorker.h"
#include "server/namespace-stuffs.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/dto/DialogDto.h"
#include "server/ws/dto/JobCreatedDto.h"
#include "server/ws/dto/StatusDto.h"

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
class DialogController : public oatpp::web::server::api::ApiController {
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
        info->addResponse<Object<JobCreatedDto>>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog", submitDialog, BODY_DTO(Object<DialogRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog", "POST", "api/v1/animation/dialog", "submitDialog", "DialogController",
            request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                // Cheap up-front shape checks. Anything that requires DB
                // access (creature existence, distinct lanes, etc.) lives in
                // the worker — failing fast on cheap stuff keeps the client
                // from having to wait + poll for the obvious "you forgot
                // turns[]" case.
                if (!requestBody) {
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = 400;
                    err->message = "request body required";
                    if (span)
                        span->setHttpStatus(400);
                    return createDtoResponse(Status::CODE_400, err);
                }
                if (!requestBody->turns || requestBody->turns->empty()) {
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = 400;
                    err->message = "turns must be a non-empty array";
                    if (span)
                        span->setHttpStatus(400);
                    return createDtoResponse(Status::CODE_400, err);
                }
                if (!requestBody->persistence ||
                    (*requestBody->persistence != "adhoc" && *requestBody->persistence != "permanent")) {
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = 400;
                    err->message = "persistence must be 'adhoc' or 'permanent'";
                    if (span)
                        span->setHttpStatus(400);
                    return createDtoResponse(Status::CODE_400, err);
                }
                for (const auto &t : *requestBody->turns) {
                    if (!t || !t->creature_id || t->creature_id->empty() || !t->text || t->text->empty()) {
                        auto err = StatusDto::createShared();
                        err->status = "error";
                        err->code = 400;
                        err->message = "every turn must have a non-empty creature_id and text";
                        if (span)
                            span->setHttpStatus(400);
                        return createDtoResponse(Status::CODE_400, err);
                    }
                }

                // Serialize the DTO into a JSON string for the job framework's
                // string-typed `details` field. The worker round-trips back
                // through the same ObjectMapper so both sides share one schema
                // (rather than agreeing on key names by hand).
                auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
                std::string detailsStr;
                try {
                    detailsStr = jsonMapper->writeToString(requestBody)->c_str();
                } catch (const std::exception &e) {
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = 500;
                    err->message = fmt::format("failed to serialize request body: {}", e.what()).c_str();
                    if (span) {
                        span->setError(e.what());
                        span->setHttpStatus(500);
                    }
                    return createDtoResponse(Status::CODE_500, err);
                }

                if (span) {
                    span->setAttribute("dialog.turns", static_cast<int64_t>(requestBody->turns->size()));
                    span->setAttribute("dialog.persistence", std::string(*requestBody->persistence));
                    span->setAttribute("dialog.autoplay",
                                       requestBody->autoplay ? static_cast<bool>(*requestBody->autoplay) : false);
                }

                const std::string jobId =
                    creatures::jobManager->createJob(creatures::jobs::JobType::Dialog, detailsStr, span);
                creatures::jobWorker->queueJob(jobId);

                if (span) {
                    span->setAttribute("job.id", jobId);
                    span->setHttpStatus(202);
                }

                auto response = JobCreatedDto::createShared();
                response->job_id = jobId.c_str();
                response->job_type = "dialog";
                response->message = fmt::format("Dialog job created with {} turn(s). Listen for job-progress and "
                                                "job-complete WebSocket messages on this job_id.",
                                                requestBody->turns->size())
                                        .c_str();
                return createDtoResponse(Status::CODE_202, response);
            });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
