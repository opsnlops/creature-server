
#include "JobWorker.h"

#include <chrono>

#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/namespace-stuffs.h"
#include "server/voice/LipSyncProcessor.h"
#include "util/ObservabilityManager.h"
#include "util/threadName.h"
#include "util/websocketUtils.h"

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::jobs {

JobWorker::JobWorker(std::shared_ptr<JobManager> jobManager)
    : jobManager_(jobManager), jobQueue_(std::make_shared<moodycamel::BlockingConcurrentQueue<std::string>>()) {
    info("JobWorker created");
}

void JobWorker::queueJob(const std::string &jobId) {
    jobQueue_->enqueue(jobId);
    info("Job {} queued for processing", jobId);
}

void JobWorker::run() {
    setThreadName("JobWorker");
    info("JobWorker thread started");

    std::string jobId;

    while (!stop_requested.load()) {
        // Wait for a job with a timeout so we can check stop_requested
        if (jobQueue_->wait_dequeue_timed(jobId, std::chrono::milliseconds(500))) {
            info("Dequeued job {} for processing", jobId);
            processJob(jobId);
        }
    }

    info("JobWorker thread stopping");
}

void JobWorker::processJob(const std::string &jobId) {
    debug("JobWorker::processJob() called for job {}", jobId);

    // Get the job state
    debug("Retrieving job state for {}", jobId);
    auto jobStateOpt = jobManager_->getJob(jobId);
    if (!jobStateOpt) {
        error("Job {} not found in JobManager", jobId);
        return;
    }

    JobState jobState = *jobStateOpt;
    info("Retrieved job state for {}: type={}, status={}, details={}", jobId, toString(jobState.jobType),
         toString(jobState.status), jobState.details);

    // Mark the job as running
    debug("Marking job {} as running", jobId);
    jobManager_->updateJobStatus(jobId, JobStatus::Running);
    info("Job {} is now running", jobId);

    try {
        // Dispatch to the appropriate handler based on job type
        debug("Dispatching job {} to handler for type {}", jobId, toString(jobState.jobType));
        switch (jobState.jobType) {
        case JobType::LipSync:
            info("Handling job {} as LipSync type", jobId);
            handleLipSyncJob(jobState);
            break;
        default:
            error("Unknown job type for job {}: {}", jobId, toString(jobState.jobType));
            jobManager_->failJob(jobId, "Unknown job type");
            return;
        }
    } catch (const std::exception &e) {
        error("Exception while processing job {}: {}", jobId, e.what());
        jobManager_->failJob(jobId, fmt::format("Exception: {}", e.what()));
    }

    debug("JobWorker::processJob() completed for job {}", jobId);
}

void JobWorker::handleLipSyncJob(JobState &jobState) {
    std::string soundFile = jobState.details;
    info("handleLipSyncJob() called for job {} with sound file: {}", jobState.jobId, soundFile);

    // Get configuration
    std::string soundsDir = config->getSoundFileLocation();
    std::string rhubarbBinaryPath = config->getRhubarbBinaryPath();

    debug("Using sounds directory: {}", soundsDir);
    debug("Using Rhubarb binary: {}", rhubarbBinaryPath);

    // Create a progress callback that updates the job
    auto progressCallback = [this, &jobState](float progress) {
        debug("Job {} progress update: {:.1f}%", jobState.jobId, progress * 100.0f);
        jobManager_->updateJobProgress(jobState.jobId, progress);

        // Broadcast progress to WebSocket clients
        auto updatedJobState = jobManager_->getJob(jobState.jobId);
        if (updatedJobState) {
            auto result = broadcastJobProgressToAllClients(*updatedJobState);
            if (!result.isSuccess()) {
                auto error = result.getError().value();
                warn("Failed to broadcast job progress: {}", error.getMessage());
            }
        }
    };

    debug("Calling LipSyncProcessor::generateLipSync for job {}", jobState.jobId);
    debug("Job span exists: {}", jobState.span ? "yes" : "no");

    // Call the LipSyncProcessor to do the actual work
    // The job's span is passed as the parent, so all LipSyncProcessor spans will be children
    auto result = voice::LipSyncProcessor::generateLipSync(soundFile, soundsDir, rhubarbBinaryPath,
                                                            true, // allowOverwrite - jobs always overwrite
                                                            progressCallback, jobState.span);

    debug("LipSyncProcessor::generateLipSync returned for job {}", jobState.jobId);

    if (result.isSuccess()) {
        // Success - mark the job as completed with the JSON result
        auto jsonContent = result.getValue().value();
        info("Job {} completed successfully, result size: {} bytes", jobState.jobId, jsonContent.size());
        jobManager_->completeJob(jobState.jobId, jsonContent);

        // Broadcast job completion to WebSocket clients
        auto completedJobState = jobManager_->getJob(jobState.jobId);
        if (completedJobState) {
            auto broadcastResult = broadcastJobCompleteToAllClients(*completedJobState);
            if (!broadcastResult.isSuccess()) {
                auto error = broadcastResult.getError().value();
                warn("Failed to broadcast job completion: {}", error.getMessage());
            }
        }

        // Schedule cache invalidation event for sound list
        scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::SoundList);

    } else {
        // Failure - mark the job as failed with the error message
        auto errorResult = result.getError().value();
        error("Job {} failed: {}", jobState.jobId, errorResult.getMessage());
        jobManager_->failJob(jobState.jobId, errorResult.getMessage());

        // Broadcast job failure to WebSocket clients
        auto failedJobState = jobManager_->getJob(jobState.jobId);
        if (failedJobState) {
            auto broadcastResult = broadcastJobCompleteToAllClients(*failedJobState);
            if (!broadcastResult.isSuccess()) {
                auto error = broadcastResult.getError().value();
                warn("Failed to broadcast job failure: {}", error.getMessage());
            }
        }
    }

    debug("handleLipSyncJob() finished for job {}", jobState.jobId);
}

} // namespace creatures::jobs
