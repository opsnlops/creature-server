
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * DTO returned when a job is created
 */
class JobCreatedDto : public oatpp::DTO {

    DTO_INIT(JobCreatedDto, DTO)

    DTO_FIELD_INFO(job_id) { info->description = "Unique job identifier (UUID)"; }
    DTO_FIELD(String, job_id);

    DTO_FIELD_INFO(job_type) { info->description = "Type of job (e.g., 'lip-sync')"; }
    DTO_FIELD(String, job_type);

    DTO_FIELD_INFO(message) { info->description = "Human-readable message"; }
    DTO_FIELD(String, message);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
