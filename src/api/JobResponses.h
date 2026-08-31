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

} // namespace creatures::api
