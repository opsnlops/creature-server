
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * DTO for job progress updates sent via WebSocket
 */
class JobProgressDto : public oatpp::DTO {

    DTO_INIT(JobProgressDto, DTO)

    DTO_FIELD_INFO(job_id) { info->description = "Unique job identifier (UUID)"; }
    DTO_FIELD(String, job_id);

    DTO_FIELD_INFO(job_type) { info->description = "Type of job (e.g., 'lip-sync')"; }
    DTO_FIELD(String, job_type);

    DTO_FIELD_INFO(status) { info->description = "Current job status (queued, running, completed, failed)"; }
    DTO_FIELD(String, status);

    DTO_FIELD_INFO(progress) { info->description = "Progress from 0.0 to 1.0"; }
    DTO_FIELD(Float32, progress);

    DTO_FIELD_INFO(details) { info->description = "Additional details about the job"; }
    DTO_FIELD(String, details);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
