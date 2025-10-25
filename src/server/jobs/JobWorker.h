
#pragma once

#include <memory>

#include "blockingconcurrentqueue.h"

#include "JobManager.h"
#include "util/StoppableThread.h"

namespace creatures::jobs {

/**
 * JobWorker
 *
 * Background thread that processes jobs from a queue.
 * Jobs are processed one at a time to avoid overwhelming the system,
 * since operations like Rhubarb lip sync are CPU-intensive and could
 * interfere with the 1ms event loop if run concurrently.
 *
 * The worker thread:
 * - Polls a concurrent queue for job IDs
 * - Retrieves job state from the JobManager
 * - Dispatches to the appropriate handler based on JobType
 * - Updates progress and broadcasts to WebSocket clients
 * - Marks jobs as completed or failed
 */
class JobWorker : public creatures::StoppableThread {
  public:
    /**
     * Create a new job worker
     *
     * @param jobManager The JobManager instance to use for job state
     */
    explicit JobWorker(std::shared_ptr<JobManager> jobManager);
    ~JobWorker() override = default;

    /**
     * Queue a job for processing
     *
     * The job will be processed in the order it was queued.
     *
     * @param jobId The unique job ID to process
     */
    void queueJob(const std::string &jobId);

  protected:
    /**
     * Main worker loop
     *
     * Continuously polls the queue for jobs and processes them one at a time.
     */
    void run() override;

  private:
    std::shared_ptr<JobManager> jobManager_;
    std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> jobQueue_;

    /**
     * Process a single job by dispatching to the appropriate handler
     *
     * @param jobId The job ID to process
     */
    void processJob(const std::string &jobId);

    /**
     * Execute a lip sync job using Rhubarb
     *
     * @param jobState The job state containing details about the job
     */
    void handleLipSyncJob(JobState &jobState);

    /**
     * Execute an ad-hoc speech animation job (speech + animation + interrupt).
     */
    void handleAdHocSpeechJob(JobState &jobState);
};

} // namespace creatures::jobs
