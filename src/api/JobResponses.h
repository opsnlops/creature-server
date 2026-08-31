#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace creatures::api {

struct JobCreatedResponse {
    std::string jobId;
    std::string jobType;
    std::string message;
};

inline nlohmann::json jobCreatedResponseToJson(const JobCreatedResponse &response) {
    return {{"job_id", response.jobId}, {"job_type", response.jobType}, {"message", response.message}};
}

struct JobStateResponse {
    std::string jobId;
    std::string jobType;
    std::string status;
    float progress;
    std::string result;
    std::string details;
};

inline nlohmann::json jobStateResponseToJson(const JobStateResponse &response) {
    return {{"job_id", response.jobId},      {"job_type", response.jobType}, {"status", response.status},
            {"progress", response.progress}, {"result", response.result},    {"details", response.details}};
}

} // namespace creatures::api
