
#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <uuid/uuid.h>

#include "JobState.h"

namespace creatures::jobs {

/**
 * JobManager
 *
 * Manages background job state and lifecycle.
 * This class is thread-safe and can be accessed from multiple threads.
 *
 * Responsibilities:
 * - Generate unique job IDs
 * - Store and retrieve job state
 * - Clean up old completed jobs
 * - Provide thread-safe access to job information
 */
class JobManager {
  public:
    JobManager() = default;
    ~JobManager() = default;

    // Delete copy constructor and assignment operator
    JobManager(const JobManager &) = delete;
    JobManager &operator=(const JobManager &) = delete;

    /**
     * Create a new job with a unique ID
     *
     * @param type The type of job to create
     * @param details Additional details about the job (e.g., filename)
     * @return The unique job ID (UUID)
     */
    std::string createJob(JobType type, const std::string &details);

    /**
     * Get the current state of a job
     *
     * @param jobId The unique job ID
     * @return The job state if found, std::nullopt otherwise
     */
    std::optional<JobState> getJob(const std::string &jobId);

    /**
     * Update the status of a job
     *
     * @param jobId The unique job ID
     * @param status The new status
     */
    void updateJobStatus(const std::string &jobId, JobStatus status);

    /**
     * Update the progress of a job
     *
     * @param jobId The unique job ID
     * @param progress The new progress (0.0 to 1.0)
     */
    void updateJobProgress(const std::string &jobId, float progress);

    /**
     * Mark a job as completed successfully
     *
     * @param jobId The unique job ID
     * @param result The result of the job (typically JSON)
     */
    void completeJob(const std::string &jobId, const std::string &result);

    /**
     * Mark a job as failed
     *
     * @param jobId The unique job ID
     * @param errorMessage The error message describing the failure
     */
    void failJob(const std::string &jobId, const std::string &errorMessage);

    /**
     * Remove jobs that have been completed for more than the specified duration
     *
     * @param olderThan Remove jobs completed before this duration ago (default: 1 hour)
     */
    void cleanupOldJobs(std::chrono::seconds olderThan = std::chrono::hours(1));

  private:
    /**
     * Generate a new UUID string
     *
     * @return A new UUID in string format
     */
    std::string generateUUID();

    std::mutex mutex_;                        // Protects access to jobs map
    std::map<std::string, JobState> jobs_;    // Map of job ID to job state
};

} // namespace creatures::jobs
