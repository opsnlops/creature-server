
#pragma once

#include <memory>

#include <fmt/format.h>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "server/jobs/JobManager.h"
#include "server/jobs/JobState.h"
#include "server/namespace-stuffs.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/dto/JobStateDto.h"
#include "server/ws/dto/StatusDto.h"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures {
extern std::shared_ptr<jobs::JobManager> jobManager;
} // namespace creatures

namespace creatures::ws {

/// Small read-only HTTP surface over the JobManager's state.
///
/// The job system is push-based (progress + completion arrive over the
/// WebSocket), but clients without a WebSocket in their REST flow — the CLI in
/// particular — need a way to poll. GET /api/v1/job/{jobId} returns the current
/// JobState (status/progress/result/details), or 404 if the id is unknown.
class JobController : public oatpp::web::server::api::ApiController, public HttpResponseHelpers<JobController> {
  public:
    JobController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) : ApiController(objectMapper) {}

    static std::shared_ptr<JobController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) {
        return std::make_shared<JobController>(objectMapper);
    }

    ENDPOINT_INFO(getJob) {
        info->summary = "Get the current state of a background job";
        info->description = "Returns the JobState (job_id, job_type, status, progress, result, details) for a job "
                            "created by one of the async endpoints. Primarily for polling clients (e.g. the CLI). "
                            "404 if the job id is unknown (never existed or already cleaned up).";
        info->addTag("Jobs");
        info->pathParams["jobId"].description = "UUID of the job, from the JobCreatedDto returned at submission.";
        info->addResponse<Object<JobStateDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/job/{jobId}", getJob, PATH(String, jobId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/job/{jobId}", "GET", "api/v1/job/{jobId}", "getJob", "JobController", request,
                           [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               const std::string id = jobId ? std::string(*jobId) : std::string();
                               if (id.empty()) {
                                   return bailHttp(span, Status::CODE_400, "jobId is required");
                               }
                               auto jobStateOpt = creatures::jobManager->getJob(id);
                               if (!jobStateOpt) {
                                   return bailHttp(span, Status::CODE_404, fmt::format("job '{}' not found", id));
                               }
                               const auto &jobState = *jobStateOpt;

                               auto dto = JobStateDto::createShared();
                               dto->job_id = jobState.jobId.c_str();
                               dto->job_type = creatures::jobs::toString(jobState.jobType).c_str();
                               dto->status = creatures::jobs::toString(jobState.status).c_str();
                               dto->progress = jobState.progress;
                               dto->result = jobState.result.c_str();
                               dto->details = jobState.details.c_str();

                               if (span) {
                                   span->setAttribute("job.id", id);
                                   span->setAttribute("job.type", creatures::jobs::toString(jobState.jobType));
                                   span->setAttribute("job.status", creatures::jobs::toString(jobState.status));
                                   span->setHttpStatus(200);
                               }
                               return createDtoResponse(Status::CODE_200, dto);
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
