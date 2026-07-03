
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * DTO snapshot of a background job's state, returned by GET /api/v1/job/{jobId}.
 *
 * Primarily for clients that poll (e.g. the CLI, which has no WebSocket in its
 * REST flows) and for debugging. Mirrors the JobProgressDto/JobCompleteDto
 * shapes but carries the full state in one object.
 */
class JobStateDto : public oatpp::DTO {

    DTO_INIT(JobStateDto, DTO)

    DTO_FIELD_INFO(job_id) { info->description = "Unique job identifier (UUID)"; }
    DTO_FIELD(String, job_id);

    DTO_FIELD_INFO(job_type) { info->description = "Type of job (e.g., 'dialog-preview')"; }
    DTO_FIELD(String, job_type);

    DTO_FIELD_INFO(status) { info->description = "Current job status (queued, running, completed, failed)"; }
    DTO_FIELD(String, status);

    DTO_FIELD_INFO(progress) { info->description = "Progress from 0.0 to 1.0"; }
    DTO_FIELD(Float32, progress);

    DTO_FIELD_INFO(result) { info->description = "Result data (JSON) on success, or error message on failure"; }
    DTO_FIELD(String, result);

    DTO_FIELD_INFO(details) { info->description = "Additional details about the job (the serialized request)"; }
    DTO_FIELD(String, details);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
