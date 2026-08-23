#pragma once

#include <nlohmann/json.hpp>

#include "server/jobs/JobState.h"

namespace creatures::jobs {

inline nlohmann::json jobProgressToJson(const JobState &jobState) {
    return {{"job_id", jobState.jobId},
            {"job_type", toString(jobState.jobType)},
            {"status", toString(jobState.status)},
            {"progress", jobState.progress},
            {"details", jobState.details}};
}

inline nlohmann::json jobCompleteToJson(const JobState &jobState) {
    return {{"job_id", jobState.jobId},
            {"job_type", toString(jobState.jobType)},
            {"status", toString(jobState.status)},
            {"result", jobState.result},
            {"details", jobState.details}};
}

} // namespace creatures::jobs
