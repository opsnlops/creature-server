
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

    /**
     * Regenerate lip sync data for an existing animation.
     */
    void handleAnimationLipSyncJob(JobState &jobState);

    /**
     * Generate a multi-character dialog scene end-to-end:
     * Text-to-Dialogue + forced-alignment + per-creature slice/timeline
     * assembly + 17-channel WAV + per-creature Tracks (with neutral-stance
     * silent turns) + Animation persistence + optional autoplay.
     *
     * Details JSON shape: { turns: [{creature_id, text}], persistence:
     * "adhoc"|"permanent", autoplay: bool, title: string }.
     */
    void handleDialogJob(JobState &jobState);

    /**
     * Generate (or load) a dialog preview take and return its metadata.
     *
     * Details JSON is a serialized DialogPreviewRequestDto. Runs the shared
     * DialogPreviewService::loadOrGenerate with per-chunk progress broadcasts;
     * completion result is the DialogPreviewMetaResponseDto JSON the sync path
     * used to return.
     */
    void handleDialogPreviewJob(JobState &jobState);

    /**
     * Assemble a dialog preview's 17-channel WAV into the ad-hoc sound bucket.
     *
     * Details JSON is a serialized DialogPreviewRequestDto. Generates/loads the
     * take, then writes the WAV to preview-exports/. Completion result carries
     * the downloadable file_name (+ generation_id, cache_key).
     */
    void handleDialogPreviewExportJob(JobState &jobState);

    /**
     * Single-voice TTS of text into a permanent sound file.
     *
     * Details JSON is a serialized MakeSoundFileRequestDto. Completion result
     * is the CreatureSpeechResponseDto JSON; fires a SoundList cache
     * invalidation on success.
     */
    void handleVoiceFileJob(JobState &jobState);
};

} // namespace creatures::jobs
