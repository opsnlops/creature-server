
#pragma once

#include <atomic>
#include <memory>
#include <thread>

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
    ~JobWorker() override;

    void start() override;
    void shutdown();

    /**
     * Queue a job for processing
     *
     * The job will be processed in the order it was queued.
     *
     * @param jobId The unique job ID to process
     */
    void queueJob(const std::string &jobId);

    /// Reserve one of the bounded music-generation slots and queue the job on
    /// its dedicated worker. Returns false without queueing when both slots are
    /// already queued/running.
    [[nodiscard]] bool tryQueueMusicJob(const std::string &jobId);

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
    std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> musicJobQueue_;
    std::atomic<std::size_t> musicJobsInFlight_{0};
    std::thread musicThread_;

    static constexpr std::size_t kMaxMusicJobsInFlight = 2;

    void runMusicJobs();

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

    /// Rebuild existing dialog animations' motion against a changed Stage.
    ///
    /// MOTION ONLY: this job must never call ElevenLabs. eleven_v3 is
    /// nondeterministic and exposes no seed, so regenerating audio for a
    /// geometry change would produce a different performance — the show would
    /// change because someone nudged a perch. The existing WAV is reused
    /// untouched; only the animation's tracks are rebuilt.
    ///
    /// Details payload: {"animation_ids": ["..."], "stage_id": "..."}.
    /// An empty stage_id re-renders each animation against its own recorded
    /// source_stage_id.
    void handleStageRerenderJob(JobState &jobState);

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

    /// Generate and cache an instrumental ElevenLabs Music take for a dialog.
    void handleDialogMusicJob(JobState &jobState);

    /**
     * Single-voice TTS of text into a permanent sound file.
     *
     * Details JSON is a serialized MakeSoundFileRequestDto. Completion result
     * is the CreatureSpeechResponseDto JSON; fires a SoundList cache
     * invalidation on success.
     */
    void handleVoiceFileJob(JobState &jobState);

    /**
     * Finish accepting a voice take whose 17-channel audio isn't on disk yet.
     *
     * Details JSON is {script_id, generation_id, dialog_cache_key}. Assembles
     * the take's WAV from the cached generation — never an ElevenLabs call, so
     * the performance is unchanged — then demotes any outgoing take, promotes
     * this one, and stamps `accepted_voice` on the script. Completion result is
     * the canonical DialogScriptDto JSON, the same body the synchronous accept
     * returns. The controller only enqueues this when the audio is missing; the
     * ordinary path stays a 200.
     */
    void handleVoiceTakeAcceptJob(JobState &jobState);
};

} // namespace creatures::jobs
