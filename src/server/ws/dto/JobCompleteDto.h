
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * DTO for job completion messages sent via WebSocket
 */
class JobCompleteDto : public oatpp::DTO {

    DTO_INIT(JobCompleteDto, DTO)

    DTO_FIELD_INFO(job_id) { info->description = "Unique job identifier (UUID)"; }
    DTO_FIELD(String, job_id);

    DTO_FIELD_INFO(job_type) { info->description = "Type of job (e.g., 'lip-sync')"; }
    DTO_FIELD(String, job_type);

    DTO_FIELD_INFO(status) { info->description = "Final job status (completed or failed)"; }
    DTO_FIELD(String, status);

    DTO_FIELD_INFO(result) { info->description = "Result data on success, or error message on failure"; }
    DTO_FIELD(String, result);

    DTO_FIELD_INFO(details) { info->description = "Additional details about the job"; }
    DTO_FIELD(String, details);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
