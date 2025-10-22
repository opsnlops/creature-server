
#include "JobManager.h"

#include "server/namespace-stuffs.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::jobs {

std::string JobManager::createJob(JobType type, const std::string &details) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string jobId = util::generateUUID();
    JobState job(jobId, type, details);

    // Create a parent span that will live for the entire job lifecycle
    // Child operations will attach to this span
    job.span = observability->createOperationSpan("Job." + toString(type));
    if (job.span) {
        job.span->setAttribute("job.id", jobId);
        job.span->setAttribute("job.type", toString(type));
        job.span->setAttribute("job.details", details);
        job.span->setAttribute("job.status", toString(JobStatus::Queued));
    }

    jobs_[jobId] = job;

    debug("Created job {} of type {} with details: {}", jobId, toString(type), details);

    return jobId;
}

std::optional<JobState> JobManager::getJob(const std::string &jobId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = jobs_.find(jobId);
    if (it != jobs_.end()) {
        return it->second;
    }

    return std::nullopt;
}

void JobManager::updateJobStatus(const std::string &jobId, JobStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = jobs_.find(jobId);
    if (it != jobs_.end()) {
        it->second.status = status;

        // Update timing information
        if (status == JobStatus::Running && it->second.startedAt.time_since_epoch().count() == 0) {
            it->second.startedAt = std::chrono::system_clock::now();
        } else if ((status == JobStatus::Completed || status == JobStatus::Failed) &&
                   it->second.completedAt.time_since_epoch().count() == 0) {
            it->second.completedAt = std::chrono::system_clock::now();
        }

        // Update the parent span with new status
        if (it->second.span) {
            it->second.span->setAttribute("job.status", toString(status));
        }

        debug("Job {} status updated to {}", jobId, toString(status));
    } else {
        warn("Attempted to update status for non-existent job: {}", jobId);
    }
}

void JobManager::updateJobProgress(const std::string &jobId, float progress) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = jobs_.find(jobId);
    if (it != jobs_.end()) {
        it->second.progress = progress;

        // Update the parent span with new progress
        if (it->second.span) {
            it->second.span->setAttribute("job.progress", static_cast<double>(progress));
        }

        debug("Job {} progress updated to {:.1f}%", jobId, progress * 100.0f);
    } else {
        warn("Attempted to update progress for non-existent job: {}", jobId);
    }
}

void JobManager::completeJob(const std::string &jobId, const std::string &result) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = jobs_.find(jobId);
    if (it != jobs_.end()) {
        it->second.status = JobStatus::Completed;
        it->second.progress = 1.0f;
        it->second.result = result;
        it->second.completedAt = std::chrono::system_clock::now();

        // Mark the parent span as successful
        if (it->second.span) {
            it->second.span->setAttribute("job.status", toString(JobStatus::Completed));
            it->second.span->setAttribute("job.result_size", static_cast<int64_t>(result.size()));
            it->second.span->setSuccess();
        }

        info("Job {} completed successfully", jobId);
    } else {
        warn("Attempted to complete non-existent job: {}", jobId);
    }
}

void JobManager::failJob(const std::string &jobId, const std::string &errorMessage) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = jobs_.find(jobId);
    if (it != jobs_.end()) {
        it->second.status = JobStatus::Failed;
        it->second.result = errorMessage;
        it->second.completedAt = std::chrono::system_clock::now();

        // Mark the parent span as failed
        if (it->second.span) {
            it->second.span->setAttribute("job.status", toString(JobStatus::Failed));
            it->second.span->setError(errorMessage);
        }

        error("Job {} failed: {}", jobId, errorMessage);
    } else {
        warn("Attempted to fail non-existent job: {}", jobId);
    }
}

void JobManager::cleanupOldJobs(std::chrono::seconds olderThan) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::system_clock::now();
    size_t removedCount = 0;

    for (auto it = jobs_.begin(); it != jobs_.end();) {
        const auto &job = it->second;

        // Only clean up completed or failed jobs
        if ((job.status == JobStatus::Completed || job.status == JobStatus::Failed) &&
            job.completedAt.time_since_epoch().count() > 0) {

            auto jobAge = std::chrono::duration_cast<std::chrono::seconds>(now - job.completedAt);

            if (jobAge > olderThan) {
                debug("Removing old job {} (age: {}s)", it->first, jobAge.count());
                // The parent span and all its children will be automatically cleaned up
                // when the JobState is destroyed
                it = jobs_.erase(it);
                removedCount++;
                continue;
            }
        }

        ++it;
    }

    if (removedCount > 0) {
        info("Cleaned up {} old job{}", removedCount, removedCount != 1 ? "s" : "");
    }
}

} // namespace creatures::jobs
