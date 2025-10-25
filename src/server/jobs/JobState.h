
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "util/ObservabilityManager.h"

namespace creatures::jobs {

/**
 * Status of a background job
 */
enum class JobStatus {
    Queued,    // Job is waiting to be processed
    Running,   // Job is currently being executed
    Completed, // Job finished successfully
    Failed     // Job failed with an error
};

/**
 * Type of background job
 */
enum class JobType {
    LipSync,     // Generate lip sync data using Rhubarb
    AdHocSpeech, // Generate ad-hoc speech animation
};

/**
 * State of a background job
 *
 * This structure tracks the complete state of a long-running background job.
 * Jobs are created when an API endpoint needs to perform a time-consuming
 * operation asynchronously.
 */
struct JobState {
    std::string jobId;   // Unique identifier for this job (UUID)
    JobType jobType;     // Type of job being performed
    JobStatus status;    // Current status of the job
    float progress;      // Progress from 0.0 to 1.0 (0% to 100%)
    std::string result;  // Final result (JSON) or error message
    std::string details; // Additional details about the job (e.g., file being processed)

    // Timing information
    std::chrono::system_clock::time_point createdAt;   // When the job was created
    std::chrono::system_clock::time_point startedAt;   // When processing began
    std::chrono::system_clock::time_point completedAt; // When the job finished

    // Observability - span that lives for the entire job lifecycle
    std::shared_ptr<creatures::OperationSpan> span;

    JobState()
        : jobId(""), jobType(JobType::LipSync), status(JobStatus::Queued), progress(0.0f), result(""), details(""),
          createdAt(std::chrono::system_clock::now()), startedAt(), completedAt(), span(nullptr) {}

    JobState(const std::string &id, JobType type, const std::string &jobDetails)
        : jobId(id), jobType(type), status(JobStatus::Queued), progress(0.0f), result(""), details(jobDetails),
          createdAt(std::chrono::system_clock::now()), startedAt(), completedAt(), span(nullptr) {}
};

/**
 * Convert JobStatus to string for logging and serialization
 */
inline std::string toString(JobStatus status) {
    switch (status) {
    case JobStatus::Queued:
        return "queued";
    case JobStatus::Running:
        return "running";
    case JobStatus::Completed:
        return "completed";
    case JobStatus::Failed:
        return "failed";
    default:
        return "unknown";
    }
}

/**
 * Convert JobType to string for logging and serialization
 */
inline std::string toString(JobType type) {
    switch (type) {
    case JobType::LipSync:
        return "lip-sync";
    case JobType::AdHocSpeech:
        return "ad-hoc-speech";
    default:
        return "unknown";
    }
}

} // namespace creatures::jobs
