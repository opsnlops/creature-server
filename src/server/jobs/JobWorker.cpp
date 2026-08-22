
#include "JobWorker.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <random>
#include <unordered_map>

#include <base64.hpp>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "model/Animation.h"
#include "model/Stage.h"
#include "server/animation/SessionManager.h"
#include "server/audio/MonoWavDownmixer.h"
#include "server/audio/SoundPathResolver.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/creature/UniverseResolver.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/storage/Storage.h"
#include "server/voice/DialogAnimation.h"
#include "server/voice/DialogCache.h"
#include "server/voice/DialogClient.h"
#include "server/voice/DialogPipeline.h"
#include "server/voice/DialogPreviewAssembly.h"
#include "server/voice/DialogWav.h"
#include "server/voice/GazeTrack.h"
#include "server/voice/IxmlReader.h"
#include "server/voice/LipSyncProcessor.h"
#include "server/voice/MusicClient.h"
#include "server/voice/RhubarbData.h"
#include "server/voice/ScriptCacheKey.h"
#include "server/voice/SoundDataProcessor.h"
#include "server/voice/SpeechGenerationManager.h"
#include "server/voice/SpeechTrackBuilder.h"
#include "server/voice/StreamingSpeechGenerationManager.h"
#include "server/voice/TextToViseme.h"
#include "server/voice/WavFileReader.h"
#include "server/ws/dto/DialogDto.h"
#include "server/ws/dto/DialogMusicDto.h"
#include "server/ws/dto/DialogPreviewExportResultDto.h"
#include "server/ws/dto/MakeSoundFileRequestDto.h"
#include "server/ws/service/DialogMusicService.h"
#include "server/ws/service/DialogPreviewService.h"
#include "server/ws/service/VoiceService.h"

#include <model/CreatureSpeechResponse.h>

#include "util/ObservabilityManager.h"
#include "util/Sha256.h"
#include "util/Slugify.h"
#include "util/cache.h"
#include "util/helpers.h"
#include "util/threadName.h"
#include "util/uuidUtils.h"
#include "util/websocketUtils.h"
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<util::AudioCache> audioCache;
} // namespace creatures

namespace creatures::jobs {

namespace {

class RemoveFileUnlessReleased {
  public:
    explicit RemoveFileUnlessReleased(std::filesystem::path path) : path_(std::move(path)) {}
    RemoveFileUnlessReleased(const RemoveFileUnlessReleased &) = delete;
    RemoveFileUnlessReleased &operator=(const RemoveFileUnlessReleased &) = delete;
    ~RemoveFileUnlessReleased() {
        if (!released_) {
            std::error_code errorCode;
            std::filesystem::remove(path_, errorCode);
            if (errorCode) {
                warn("Unable to remove uncommitted dialog WAV: {}", errorCode.message());
            }
        }
    }
    void release() { released_ = true; }

  private:
    std::filesystem::path path_;
    bool released_{false};
};

// getAdHocTempRoot + getAnimationLipSyncTempRoot used to live here as anon-
// namespace helpers (the latter for the lipsync handler, the former for ad-hoc
// speech + the dialog handler). All callers migrated to the storage facade in
// issue #11 — see creatures::storage::{root, allocateSoundPath}.

Result<void> prewarmAudioCache(const std::filesystem::path &wavPath, std::shared_ptr<OperationSpan> parentSpan) {
    auto span = creatures::observability->createChildOperationSpan("AdHocSpeech.prewarmAudioCache", parentSpan);
    if (span) {
        span->setAttribute("sound.path", wavPath.string());
    }

    if (wavPath.empty()) {
        std::string message = "Cannot prewarm audio cache without a WAV path";
        warn(message);
        if (span) {
            span->setError(message);
        }
        return Result<void>{ServerError(ServerError::InvalidData, message)};
    }

    if (!std::filesystem::exists(wavPath)) {
        auto message = fmt::format("Cannot prewarm cache; WAV {} does not exist", wavPath.string());
        warn(message);
        if (span) {
            span->setError(message);
        }
        return Result<void>{ServerError(ServerError::NotFound, message)};
    }

    if (!creatures::audioCache) {
        debug("Audio cache disabled, skipping prewarm for {}", wavPath.string());
        if (span) {
            span->setAttribute("cache.enabled", false);
            span->setSuccess();
        }
        return Result<void>{};
    }

    // Prewarm: this exists to populate the DISK cache, and the buffer is
    // discarded immediately — don't charge it against the in-memory retention
    // budget (issue #93).
    auto buffer = creatures::rtp::AudioStreamBuffer::loadFromWavFile(
        wavPath.string(), span, creatures::rtp::AudioStreamBuffer::RetentionIntent::OneShot);
    if (!buffer) {
        auto message = fmt::format("AudioStreamBuffer failed while prewarming {}", wavPath.string());
        warn(message);
        if (span) {
            span->setError(message);
        }
        return Result<void>{ServerError(ServerError::InternalError, message)};
    }

    if (span) {
        span->setAttribute("cache.enabled", true);
        span->setSuccess();
    }
    debug("Prewarmed audio cache for {}", wavPath.string());
    return Result<void>{};
}

} // namespace

JobWorker::JobWorker(std::shared_ptr<JobManager> jobManager)
    : jobManager_(jobManager), jobQueue_(std::make_shared<moodycamel::BlockingConcurrentQueue<std::string>>()),
      musicJobQueue_(std::make_shared<moodycamel::BlockingConcurrentQueue<std::string>>()) {
    info("JobWorker created");
}

JobWorker::~JobWorker() { shutdown(); }

void JobWorker::start() {
    StoppableThread::start();
    musicThread_ = std::thread(&JobWorker::runMusicJobs, this);
}

void JobWorker::shutdown() {
    StoppableThread::shutdown();
    if (musicThread_.joinable()) {
        musicThread_.join();
    }
}

void JobWorker::queueJob(const std::string &jobId) {
    jobQueue_->enqueue(jobId);
    info("Job {} queued for processing", jobId);
}

bool JobWorker::tryQueueMusicJob(const std::string &jobId) {
    auto current = musicJobsInFlight_.load(std::memory_order_relaxed);
    while (current < kMaxMusicJobsInFlight) {
        if (musicJobsInFlight_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
            musicJobQueue_->enqueue(jobId);
            info("Music job {} queued for dedicated processing", jobId);
            return true;
        }
    }
    return false;
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

void JobWorker::runMusicJobs() {
    setThreadName("MusicJobWorker");
    info("Music job worker thread started");
    std::string jobId;
    while (!stop_requested.load()) {
        if (musicJobQueue_->wait_dequeue_timed(jobId, std::chrono::milliseconds(500))) {
            processJob(jobId);
            musicJobsInFlight_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    info("Music job worker thread stopping");
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
    info("Retrieved job state for {}: type={}, status={}, details_length={}", jobId, toString(jobState.jobType),
         toString(jobState.status), jobState.details.size());

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
        case JobType::AdHocSpeech:
            info("Handling job {} as AdHocSpeech type", jobId);
            handleAdHocSpeechJob(jobState);
            break;
        case JobType::AdHocSpeechPrepare:
            info("Handling job {} as AdHocSpeechPrepare type", jobId);
            handleAdHocSpeechJob(jobState);
            break;
        case JobType::AnimationLipSync:
            info("Handling job {} as AnimationLipSync type", jobId);
            handleAnimationLipSyncJob(jobState);
            break;
        case JobType::Dialog:
            info("Handling job {} as Dialog type", jobId);
            handleDialogJob(jobState);
            break;
        case JobType::DialogPreview:
            info("Handling job {} as DialogPreview type", jobId);
            handleDialogPreviewJob(jobState);
            break;
        case JobType::DialogPreviewExport:
            info("Handling job {} as DialogPreviewExport type", jobId);
            handleDialogPreviewExportJob(jobState);
            break;
        case JobType::DialogMusic:
            info("Handling job {} as DialogMusic type", jobId);
            handleDialogMusicJob(jobState);
            break;
        case JobType::StageRerender:
            handleStageRerenderJob(jobState);
            break;
        case JobType::VoiceFile:
            info("Handling job {} as VoiceFile type", jobId);
            handleVoiceFileJob(jobState);
            break;
        case JobType::VoiceTakeAccept:
            info("Handling job {} as VoiceTakeAccept type", jobId);
            handleVoiceTakeAcceptJob(jobState);
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

void JobWorker::handleDialogMusicJob(JobState &jobState) {
    const auto broadcastProgress = [this, &jobState] {
        if (auto state = jobManager_->getJob(jobState.jobId)) {
            auto result = broadcastJobProgressToAllClients(*state);
            if (!result.isSuccess())
                warn("Failed to broadcast dialog music job progress: {}", result.getError()->getMessage());
        }
    };
    const auto broadcastCompletion = [this, &jobState] {
        if (auto state = jobManager_->getJob(jobState.jobId)) {
            auto result = broadcastJobCompleteToAllClients(*state);
            if (!result.isSuccess())
                warn("Failed to broadcast dialog music job completion: {}", result.getError()->getMessage());
        }
    };
    const auto failJob = [&](const std::string &message, const std::string &type, ServerError::Code code,
                             const std::string &stage) {
        recordSpanError(jobState.span, message, type, code);
        if (jobState.span)
            jobState.span->setAttribute("job.failure_stage", stage);
        jobManager_->failJob(jobState.jobId, message);
        broadcastCompletion();
    };

    auto mapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
    oatpp::Object<ws::DialogMusicRequestDto> request;
    try {
        request = mapper->readFromString<oatpp::Object<ws::DialogMusicRequestDto>>(jobState.details.c_str());
    } catch (const std::exception &e) {
        if (jobState.span)
            jobState.span->recordException(e);
        return failJob(fmt::format("invalid dialog music job details: {}", e.what()), "JsonParsingException",
                       ServerError::InvalidData, "deserialize");
    }
    if (!request) {
        return failJob("dialog music job details deserialized to null", "InvalidData", ServerError::InvalidData,
                       "deserialize");
    }
    jobManager_->updateJobProgress(jobState.jobId, 0.05f);
    broadcastProgress();

    ws::DialogMusicService service;
    auto generated = service.generate(request, jobState.span, jobState.jobId);
    if (!generated.isSuccess()) {
        const auto error = generated.getError().value();
        return failJob(error.getMessage(), "DialogMusicGenerationError", error.getCode(), "generate");
    }
    jobManager_->updateJobProgress(jobState.jobId, 1.0f);
    broadcastProgress();
    jobManager_->completeJob(jobState.jobId, mapper->writeToString(generated.getValue().value())->c_str());
    if (jobState.span)
        jobState.span->setSuccess();
    broadcastCompletion();
}

void JobWorker::handleAnimationLipSyncJob(JobState &jobState) {
    auto broadcastProgress = [this](const std::string &jobId) {
        auto updatedJobState = jobManager_->getJob(jobId);
        if (updatedJobState) {
            auto result = broadcastJobProgressToAllClients(*updatedJobState);
            if (!result.isSuccess()) {
                warn("Failed to broadcast job progress: {}", result.getError()->getMessage());
            }
        }
    };

    auto broadcastCompletion = [this](const std::string &jobId) {
        auto updatedJobState = jobManager_->getJob(jobId);
        if (updatedJobState) {
            auto result = broadcastJobCompleteToAllClients(*updatedJobState);
            if (!result.isSuccess()) {
                warn("Failed to broadcast job completion: {}", result.getError()->getMessage());
            }
        }
    };

    auto updateProgress = [&](float value) {
        jobManager_->updateJobProgress(jobState.jobId, value);
        broadcastProgress(jobState.jobId);
    };

    auto failJob = [&](const std::string &message) {
        error("Animation lip sync job {} failed: {}", jobState.jobId, message);
        jobManager_->failJob(jobState.jobId, message);
        broadcastCompletion(jobState.jobId);
    };

    std::string animationId;
    try {
        auto detailsJson = nlohmann::json::parse(jobState.details);
        animationId = detailsJson.at("animation_id").get<std::string>();
    } catch (const std::exception &e) {
        failJob(fmt::format("Invalid job details: {}", e.what()));
        return;
    }

    if (animationId.empty()) {
        failJob("animation_id is required");
        return;
    }

    if (jobState.span) {
        jobState.span->setAttribute("animation.id", animationId);
    }

    updateProgress(0.02f);

    auto animationSpan =
        creatures::observability->createChildOperationSpan("Job.AnimationLipSync.loadAnimation", jobState.span);
    auto animationResult = db->getAnimation(animationId, animationSpan);
    if (!animationResult.isSuccess()) {
        failJob(animationResult.getError()->getMessage());
        return;
    }
    auto animation = animationResult.getValue().value();

    if (animation.tracks.empty()) {
        failJob(fmt::format("Animation {} has no tracks", animationId));
        return;
    }

    if (animation.metadata.sound_file.empty()) {
        failJob(fmt::format("Animation {} has no sound file", animationId));
        return;
    }

    if (!animation.metadata.multitrack_audio) {
        failJob(fmt::format("Animation {} does not use multitrack audio", animationId));
        return;
    }

    const auto soundsDir = std::filesystem::path(config->getSoundFileLocation());
    const auto audioPath = soundsDir / animation.metadata.sound_file;
    if (!std::filesystem::exists(audioPath)) {
        failJob(fmt::format("Sound file for animation not found: {}", audioPath.string()));
        return;
    }

    // Pure-C++ WAV header read — we control the format, no ffmpeg needed
    // (issue #12 Phase B).
    auto channelCountResult = voice::readWavChannelCount(audioPath);
    if (!channelCountResult.isSuccess()) {
        failJob(channelCountResult.getError()->getMessage());
        return;
    }
    const auto channelCount = channelCountResult.getValue().value();
    if (channelCount != RTP_STREAMING_CHANNELS) {
        failJob(fmt::format("Expected {} channels but audio has {}", RTP_STREAMING_CHANNELS, channelCount));
        return;
    }

    // Per-job scratch dir under temp/creature-lipsync/<jobId>/. Cleaned up by
    // TempDirGuard below at job end (these are intermediate extraction WAVs,
    // not artifacts that need to outlive the job).
    auto scratchRootResult = creatures::storage::root(creatures::storage::Persistence::JobScratch);
    if (!scratchRootResult.isSuccess()) {
        failJob(fmt::format("Unable to access lipsync scratch root: {}", scratchRootResult.getError()->getMessage()));
        return;
    }
    auto tempDir = scratchRootResult.getValue().value() / jobState.jobId;
    std::error_code tempEc;
    std::filesystem::create_directories(tempDir, tempEc);
    if (tempEc) {
        failJob(fmt::format("Unable to create temp directory {}: {}", tempDir.string(), tempEc.message()));
        return;
    }

    struct TempDirGuard {
        std::filesystem::path path;
        ~TempDirGuard() {
            if (path.empty())
                return;
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            if (ec) {
                warn("Failed to remove temp directory {}: {}", path.string(), ec.message());
            }
        }
    } cleanup{tempDir};

    SoundDataProcessor processor;
    std::unordered_map<creatureId_t, Creature> creatureCache;

    const size_t trackCount = animation.tracks.size();
    const double baseProgress = 0.1;
    const double perTrackRange = trackCount == 0 ? 0.0 : 0.8 / static_cast<double>(trackCount);

    for (size_t idx = 0; idx < trackCount; ++idx) {
        const auto &track = animation.tracks[idx];

        if (track.frames.empty()) {
            failJob(fmt::format("Track {} has no frames", track.id));
            return;
        }

        const auto &creatureId = track.creature_id;
        if (creatureId.empty()) {
            failJob(fmt::format("Track {} has no creature_id", track.id));
            return;
        }

        Creature creature;
        auto cacheIt = creatureCache.find(creatureId);
        if (cacheIt != creatureCache.end()) {
            creature = cacheIt->second;
        } else {
            auto creatureResult = db->getCreature(creatureId, jobState.span);
            if (!creatureResult.isSuccess()) {
                failJob(
                    fmt::format("Unable to load creature {}: {}", creatureId, creatureResult.getError()->getMessage()));
                return;
            }
            creature = creatureResult.getValue().value();
            creatureCache.emplace(creatureId, creature);
        }

        if (creature.audio_channel == 0 || creature.audio_channel >= RTP_STREAMING_CHANNELS) {
            failJob(fmt::format("Creature {} has invalid audio_channel {} (1-{} expected)", creatureId,
                                creature.audio_channel, RTP_STREAMING_CHANNELS - 1));
            return;
        }

        const std::string trackSlug =
            util::slugify(creatureId.empty() ? fmt::format("track{}", idx) : creatureId, 40, "speech");
        const auto monoPath = tempDir / fmt::format("{}-ch{}.wav", trackSlug, creature.audio_channel);

        auto trackStageProgress = [&](double stage) {
            double progress = baseProgress + perTrackRange * (static_cast<double>(idx) + stage);
            updateProgress(static_cast<float>(std::min(progress, 0.95)));
        };

        trackStageProgress(0.05);

        auto extractResult =
            voice::extractChannelToMonoWav(audioPath, monoPath, static_cast<int>(creature.audio_channel));
        if (!extractResult.isSuccess()) {
            failJob(extractResult.getError()->getMessage());
            return;
        }

        auto lipSyncProgress = [&](float stage) { trackStageProgress(0.1 + 0.6 * static_cast<double>(stage)); };

        auto lipSyncResult = voice::LipSyncProcessor::generateLipSync(monoPath.filename().string(), tempDir.string(),
                                                                      config->getRhubarbBinaryPath(), true,
                                                                      lipSyncProgress, jobState.span);
        if (!lipSyncResult.isSuccess()) {
            failJob(lipSyncResult.getError()->getMessage());
            return;
        }

        auto rhubarbData = RhubarbSoundData::fromJsonString(lipSyncResult.getValue().value());

        auto trackResult = processor.replaceAxisDataWithSoundData(rhubarbData, creatures::resolvedMouthSlot(creature),
                                                                  track, animation.metadata.milliseconds_per_frame);
        if (!trackResult.isSuccess()) {
            failJob(trackResult.getError()->getMessage());
            return;
        }

        animation.tracks[idx] = trackResult.getValue().value();
        trackStageProgress(0.95);
    }

    // republishAnimation fires Animation invalidation only (no SoundList —
    // we only mutated existing tracks; the sound file reference is unchanged).
    auto animationJson = animationToJson(animation);
    auto upsertResult = creatures::storage::republishAnimation(animationJson.dump(), jobState.span);
    if (!upsertResult.isSuccess()) {
        failJob(upsertResult.getError()->getMessage());
        return;
    }
    updateProgress(0.98f);

    nlohmann::json resultJson;
    resultJson["animation_id"] = animation.id;
    resultJson["updated_tracks"] = trackCount;

    jobManager_->completeJob(jobState.jobId, resultJson.dump());
    broadcastCompletion(jobState.jobId);
}

void JobWorker::handleLipSyncJob(JobState &jobState) {
    // Parse job details JSON
    std::string soundFile;
    bool allowOverwrite = false;

    try {
        auto detailsJson = nlohmann::json::parse(jobState.details);
        soundFile = detailsJson["sound_file"].get<std::string>();
        allowOverwrite = detailsJson.value("allow_overwrite", false);
    } catch (const nlohmann::json::exception &e) {
        error("Failed to parse job details JSON for job {}: {}", jobState.jobId, e.what());
        jobManager_->failJob(jobState.jobId, fmt::format("Invalid job details: {}", e.what()));
        return;
    }

    info("handleLipSyncJob() called for job {} with sound file: {}, allow_overwrite: {}", jobState.jobId, soundFile,
         allowOverwrite);

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
                                                           allowOverwrite, // Use the allow_overwrite from the request
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

        // Invalidate the sound list through the facade so the basename index
        // is marked dirty too (issue #94).
        creatures::storage::broadcastCacheInvalidation(CacheType::SoundList);

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

void JobWorker::handleAdHocSpeechJob(JobState &jobState) {
    info("handleAdHocSpeechJob() called for job {}", jobState.jobId);

    std::string creatureId;
    std::string text;
    bool resumePlaylist = true;
    bool autoPlay = true;

    try {
        auto detailsJson = nlohmann::json::parse(jobState.details);
        creatureId = detailsJson.at("creature_id").get<std::string>();
        text = detailsJson.at("text").get<std::string>();
        resumePlaylist = detailsJson.value("resume_playlist", true);
        autoPlay = detailsJson.value("auto_play", true);
    } catch (const std::exception &e) {
        std::string msg = fmt::format("Invalid job details: {}", e.what());
        error(msg);
        jobManager_->failJob(jobState.jobId, msg);
        auto failedJobState = jobManager_->getJob(jobState.jobId);
        if (failedJobState) {
            auto broadcastResult = broadcastJobCompleteToAllClients(*failedJobState);
            if (!broadcastResult.isSuccess()) {
                warn("Failed to broadcast job failure: {}", broadcastResult.getError()->getMessage());
            }
        }
        return;
    }

    if (jobState.span) {
        jobState.span->setAttribute("creature.id", creatureId);
        // Avoid sending the full user-supplied text to Honeycomb (potential PII,
        // unbounded cardinality). The length plus a short preview is enough for
        // debugging.
        jobState.span->setAttribute("speech.text_length", static_cast<int64_t>(text.size()));
        jobState.span->setAttribute("speech.text_preview", text.substr(0, 60));
        jobState.span->setAttribute("speech.auto_play", autoPlay);
        jobState.span->setAttribute("speech.resume_playlist", resumePlaylist);
    }

    if (creatureId.empty() || text.empty()) {
        std::string msg = "Ad-hoc speech jobs require both creature_id and text";
        jobManager_->failJob(jobState.jobId, msg);
        auto failedJobState = jobManager_->getJob(jobState.jobId);
        if (failedJobState) {
            auto broadcastResult = broadcastJobCompleteToAllClients(*failedJobState);
            if (!broadcastResult.isSuccess()) {
                warn("Failed to broadcast job failure: {}", broadcastResult.getError()->getMessage());
            }
        }
        return;
    }

    auto broadcastProgress = [this](const std::string &jobId) {
        auto updatedJobState = jobManager_->getJob(jobId);
        if (updatedJobState) {
            auto result = broadcastJobProgressToAllClients(*updatedJobState);
            if (!result.isSuccess()) {
                warn("Failed to broadcast job progress: {}", result.getError()->getMessage());
            }
        }
    };

    auto broadcastCompletion = [this](const std::string &jobId) {
        auto updatedJobState = jobManager_->getJob(jobId);
        if (updatedJobState) {
            auto result = broadcastJobCompleteToAllClients(*updatedJobState);
            if (!result.isSuccess()) {
                warn("Failed to broadcast job completion: {}", result.getError()->getMessage());
            }
        }
    };

    auto updateProgress = [&](float value) {
        jobManager_->updateJobProgress(jobState.jobId, value);
        broadcastProgress(jobState.jobId);
    };

    auto failJob = [&](const std::string &message) {
        error("Ad-hoc job {} failed: {}", jobState.jobId, message);
        jobManager_->failJob(jobState.jobId, message);
        broadcastCompletion(jobState.jobId);
    };

    try {
        updateProgress(0.05f);

        // Per-job ad-hoc dir under temp/creature-adhoc/<jobId>/. Files written
        // here outlive the job (referenced from metadata.sound_file) and get
        // cleaned by the existing TTL sweep, not by the job itself.
        auto adHocRootResult = creatures::storage::root(creatures::storage::Persistence::AdHoc);
        if (!adHocRootResult.isSuccess()) {
            failJob(fmt::format("Unable to access ad-hoc root: {}", adHocRootResult.getError()->getMessage()));
            return;
        }
        auto tempDir = adHocRootResult.getValue().value() / jobState.jobId;
        std::error_code ec;
        std::filesystem::create_directories(tempDir, ec);
        if (ec) {
            failJob(fmt::format("Unable to create temp directory {}: {}", tempDir.string(), ec.message()));
            return;
        }

        voice::SpeechGenerationRequest speechRequest;
        speechRequest.creatureId = creatureId;
        speechRequest.text = text;
        speechRequest.title = fmt::format("AdHoc {}", jobState.jobId);
        speechRequest.outputDirectory = tempDir;
        speechRequest.parentSpan = jobState.span;

        // Speech generation + lip sync data
        // Try streaming path first (single WebSocket call for audio + alignment)
        // Falls back to REST TTS + whisper/rhubarb if streaming fails
        RhubarbSoundData rhubarbData;
        std::filesystem::path wavPath;
        std::filesystem::path transcriptPath;
        Creature creature;
        std::string creatureName;
        auto textSlug = util::slugify(text, 40, "speech");
        // Whole seconds only: a raw system_clock time point makes fmt's %S
        // print fractional seconds, and this timestamp lands in a FILENAME —
        // the extra dot makes the basename unservable through the rendition
        // route's one-dot sanitizer.
        auto timestamp = fmt::format(
            "{:%Y%m%d%H%M%S}", std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()));

        if (jobState.span) {
            jobState.span->setAttribute("speech.attempted_engine", std::string("websocket_streaming"));
        }

        auto streamingResult = voice::StreamingSpeechGenerationManager::generate(speechRequest);
        if (streamingResult.isSuccess()) {
            auto streamingAssets = streamingResult.getValue().value();
            info("Streaming TTS succeeded for job {} ({:.2f}s audio)", jobState.jobId,
                 streamingAssets.audioDurationSeconds);
            if (jobState.span) {
                jobState.span->setAttribute("speech.engine_used", std::string("websocket_streaming"));
                jobState.span->setAttribute("audio.duration_s", streamingAssets.audioDurationSeconds);
            }

            rhubarbData = streamingAssets.lipSyncData;
            wavPath = streamingAssets.wavPath;
            transcriptPath = streamingAssets.transcriptPath;
            creature = streamingAssets.creature;
            creatureName = creature.name.empty() ? creatureId : creature.name;
            updateProgress(0.45f);

            // Prewarm audio cache in background
            auto cacheFuture = std::async(std::launch::async, [wp = wavPath, span = jobState.span]() {
                debug("Starting background cache prewarm for {}", wp.string());
                return prewarmAudioCache(wp, span);
            });
            auto cacheResult = cacheFuture.get();
            if (!cacheResult.isSuccess()) {
                warn("Audio cache prewarm failed: {}", cacheResult.getError()->getMessage());
            }
            updateProgress(0.55f);
        } else {
            // Streaming failed — fall back to REST TTS + lip sync
            warn("Streaming TTS failed for job {}: {}, falling back to REST path", jobState.jobId,
                 streamingResult.getError()->getMessage());
            if (jobState.span) {
                jobState.span->setAttribute("speech.engine_used", std::string("rest_fallback"));
                jobState.span->setAttribute("speech.streaming_error", streamingResult.getError()->getMessage());
            }

            auto speechResult = voice::SpeechGenerationManager::generate(speechRequest);
            if (!speechResult.isSuccess()) {
                failJob(speechResult.getError()->getMessage());
                return;
            }
            updateProgress(0.2f);

            auto speechAssets = speechResult.getValue().value();
            creature = speechAssets.creature;
            creatureName = creature.name.empty() ? creatureId : creature.name;

            // Rename files with descriptive names
            auto creatureSlug = util::slugify(creatureName, 40, "speech");
            auto baseName = fmt::format("adhoc_{}_{}_{}", creatureSlug, timestamp, textSlug);

            auto renameIfExists = [&](const std::filesystem::path &oldPath, const std::string &ext) {
                if (oldPath.empty() || !std::filesystem::exists(oldPath)) {
                    return;
                }
                auto newPath = oldPath.parent_path() / fmt::format("{}.{}", baseName, ext);
                std::error_code renameEc;
                std::filesystem::rename(oldPath, newPath, renameEc);
                if (renameEc) {
                    warn("Unable to rename {} to {}: {}", oldPath.string(), newPath.string(), renameEc.message());
                    return;
                }
                if (ext == "wav") {
                    speechAssets.wavPath = newPath;
                } else if (ext == "txt") {
                    speechAssets.transcriptPath = newPath;
                }
            };

            renameIfExists(speechAssets.wavPath, "wav");
            renameIfExists(speechAssets.mp3Path, "mp3");
            renameIfExists(speechAssets.transcriptPath, "txt");

            wavPath = speechAssets.wavPath;
            transcriptPath = speechAssets.transcriptPath;

            // Prewarm audio cache + lip sync in parallel
            auto cacheFuture = std::async(std::launch::async, [wp = wavPath, span = jobState.span]() {
                debug("Starting background cache prewarm for {}", wp.string());
                return prewarmAudioCache(wp, span);
            });

            auto lipSyncProgress = [&, base = 0.2f, range = 0.3f](float p) { updateProgress(base + range * p); };

            auto lipSyncResult = voice::LipSyncProcessor::generateLipSync(wavPath.filename().string(), tempDir.string(),
                                                                          config->getRhubarbBinaryPath(), true,
                                                                          lipSyncProgress, jobState.span);

            auto cacheResult = cacheFuture.get();
            if (!cacheResult.isSuccess()) {
                warn("Audio cache prewarm failed: {}", cacheResult.getError()->getMessage());
            }
            if (!lipSyncResult.isSuccess()) {
                failJob(lipSyncResult.getError()->getMessage());
                return;
            }

            rhubarbData = RhubarbSoundData::fromJsonString(lipSyncResult.getValue().value());
            updateProgress(0.55f);
        }

        // Rename WAV/transcript with descriptive names if streaming path was used
        // (streaming path outputs generic names)
        if (streamingResult.isSuccess()) {
            auto creatureSlug = util::slugify(creatureName, 40, "speech");
            auto baseName = fmt::format("adhoc_{}_{}_{}", creatureSlug, timestamp, textSlug);

            auto renameFile = [&](std::filesystem::path &path, const std::string &ext) {
                if (path.empty() || !std::filesystem::exists(path)) {
                    return;
                }
                auto newPath = path.parent_path() / fmt::format("{}.{}", baseName, ext);
                std::error_code renameEc;
                std::filesystem::rename(path, newPath, renameEc);
                if (!renameEc) {
                    path = newPath;
                }
            };

            renameFile(wavPath, "wav");
            renameFile(transcriptPath, "txt");
        }

        if (jobState.span) {
            jobState.span->setAttribute("creature.name", creatureName);
        }

        if (creature.speech_loop_animation_ids.empty()) {
            failJob(fmt::format("Creature '{}' has no speech_loop_animation_ids configured", creatureName));
            return;
        }

        // Shared speech-loop resolution via the helper (issue #15). Picks a
        // random speech_loop_animation_ids entry, loads the animation, finds
        // the per-creature track, decodes + validates frame widths.
        std::mt19937 rng(static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        auto resolveResult = voice::resolveSpeechBaseFrames(creature, *db, rng, jobState.span);
        if (!resolveResult.isSuccess()) {
            failJob(resolveResult.getError()->getMessage());
            return;
        }
        auto resolved = resolveResult.getValue().value();
        Animation baseAnimation = std::move(resolved.baseAnimation);
        uint32_t msPerFrame = resolved.baseMsPerFrame == 0 ? 1u : resolved.baseMsPerFrame;

        size_t targetFrames = std::max<size_t>(
            1,
            static_cast<size_t>(std::ceil((rhubarbData.metadata.duration * 1000.0) / static_cast<double>(msPerFrame))));

        SoundDataProcessor processor;
        auto mouthData = processor.processSoundData(rhubarbData, msPerFrame, targetFrames);

        // Shared frame-build via the speech track builder (issue #15).
        Animation adHocAnimation = baseAnimation;
        adHocAnimation.id = util::generateUUID();
        adHocAnimation.metadata.animation_id = adHocAnimation.id;
        adHocAnimation.metadata.title = fmt::format("{} - {} - {}", creatureName, timestamp, textSlug);
        adHocAnimation.metadata.sound_file = wavPath.string();
        adHocAnimation.metadata.note = fmt::format("Ad-hoc speech generated from text: {}", text);
        adHocAnimation.metadata.number_of_frames = static_cast<uint32_t>(targetFrames);
        adHocAnimation.metadata.multitrack_audio = true;

        voice::SpeechTrackInput trackInput;
        trackInput.baseFrames = resolved.baseFrames;
        trackInput.mouthBytes = mouthData;
        trackInput.mouthSlot = creatures::resolvedMouthSlot(creature);
        trackInput.totalFrames = targetFrames;
        trackInput.creatureId = creatureId;
        trackInput.animationId = adHocAnimation.id;
        auto trackResult = voice::buildSpeechTrack(trackInput, {}, jobState.span);
        if (!trackResult.isSuccess()) {
            failJob(trackResult.getError()->getMessage());
            return;
        }
        adHocAnimation.tracks = {std::move(trackResult.getValue()->track)};

        // Storage facade pairs the DB insert + AdHocAnimationList + AdHocSoundList
        // invalidations so this handler can't forget any of the three (issue #11).
        auto insertResult = creatures::storage::publishAdHocAnimation(adHocAnimation, jobState.span);
        if (!insertResult.isSuccess()) {
            failJob(insertResult.getError()->getMessage());
            return;
        }
        updateProgress(0.85f);

        nlohmann::json completionJson;
        completionJson["animation_id"] = adHocAnimation.id;
        completionJson["sound_file"] = adHocAnimation.metadata.sound_file;
        completionJson["resume_playlist"] = resumePlaylist;
        completionJson["temp_directory"] = tempDir.string();
        completionJson["auto_play"] = autoPlay;

        bool playbackTriggered = false;
        universe_t universe{};
        if (autoPlay) {
            try {
                auto universePtr = creatureUniverseMap->get(creatureId);
                universe = *universePtr;
            } catch (const std::exception &) {
                failJob(fmt::format("Creature {} is not registered with a universe. Is the controller online?",
                                    creatureId));
                return;
            }

            auto sessionResult = sessionManager->interrupt(universe, adHocAnimation, resumePlaylist);
            if (!sessionResult.isSuccess()) {
                failJob(sessionResult.getError()->getMessage());
                return;
            }

            completionJson["universe"] = universe;
            playbackTriggered = true;
        }
        completionJson["playback_triggered"] = playbackTriggered;

        updateProgress(1.0f);
        jobManager_->completeJob(jobState.jobId, completionJson.dump());
        broadcastCompletion(jobState.jobId);
        info("Ad-hoc job {} completed successfully", jobState.jobId);

    } catch (const std::exception &e) {
        failJob(e.what());
    }
}

// ===========================================================================
// Dialog job handler — runs the Phases 1–4 pipeline end-to-end.
// ===========================================================================

namespace {

constexpr uint32_t kDialogSampleRate = 48000;

/// Subdirectory under the server's sound file location where permanent dialog
/// scenes live. metadata.sound_file stores the relative path under this dir;
/// playback layers prepend `getSoundFileLocation()` for any non-absolute path
/// (see DecodedAudioStream.cpp).
constexpr const char *kPermanentDialogSubdir = "dialog";

/// Maximum unique voice IDs per ElevenLabs Text-to-Dialogue submission. The
/// official cap applies per-call; we enforce per-scene because chunking can't
/// rescue an over-budget scene without throwing away cross-speaker reactivity.
constexpr std::size_t kMaxUniqueVoicesPerScene = 10;

/// Internal enum for which animations table the assembled scene gets
/// persisted into. The details JSON sends a string; we parse to this.
enum class DialogPersistence {
    AdHoc,     // TTL collection (insertAdHocAnimation)
    Permanent, // normal animations collection (upsertAnimation)
};

/// Resolved per-creature info, cached for the lifetime of one dialog job.
struct DialogJobCreature {
    std::string creatureId;
    nlohmann::json creatureJson; // full stored doc
    std::string voiceId;
    uint16_t audioChannel; // 1-based
    uint8_t mouthSlot;
    universe_t universe; // looked up from creatureUniverseMap; only used on autoplay
};

/// Lazy-loaded shared TextToViseme. The CMU dict is multi-MB; one load per
/// process is enough — every dialog job reuses it. Guarded by the local mutex
/// so the first concurrent jobs don't both pay the load cost.
std::shared_ptr<voice::TextToViseme> getDialogTextToViseme() {
    static std::mutex mu;
    static std::shared_ptr<voice::TextToViseme> instance;
    std::lock_guard<std::mutex> lock(mu);
    if (instance && instance->isLoaded()) {
        return instance;
    }
    auto v = std::make_shared<voice::TextToViseme>();
    const auto path = creatures::config->getCmuDictPath();
    if (path.empty() || !v->loadCmuDict(path)) {
        warn("Dialog job: CMU dict not loaded (path='{}'); viseme cues will fall back to whatever TextToViseme "
             "produces with an empty dict",
             path);
    }
    instance = v;
    return instance;
}

} // namespace

// ===========================================================================
// Stage re-render (#119)
//
// Rebuild an existing dialog animation's MOTION against a changed Stage,
// reusing its audio exactly as it is.
//
// The hard constraint: this must never call ElevenLabs. eleven_v3 is
// nondeterministic and exposes no seed, so regenerating audio because someone
// moved a perch would change the performance itself. Everything needed to
// rebuild the motion is already persisted:
//
//   * the speaking timeline and mouth bytes, recoverable from the rendered
//     WAV's iXML LIPSYNC block (falling back to scraping the existing track's
//     mouth slot);
//   * which loops each creature drew, from metadata.source_render_choices;
//   * the gaze timing seed, from metadata.render_seed.
//
// With the choices and seed replayed, a re-render against an UNCHANGED stage
// reproduces the animation byte for byte, and a re-render against a moved
// creature changes only the head-aiming bytes. That property is what makes
// this safe to run over a whole show.
// ===========================================================================
void JobWorker::handleStageRerenderJob(JobState &jobState) {
    auto broadcastProgress = [this](const std::string &jobId) {
        auto updated = jobManager_->getJob(jobId);
        if (updated) {
            auto r = broadcastJobProgressToAllClients(*updated);
            if (!r.isSuccess()) {
                warn("Failed to broadcast stage re-render progress: {}", r.getError()->getMessage());
            }
        }
    };
    auto broadcastCompletion = [this](const std::string &jobId) {
        auto updated = jobManager_->getJob(jobId);
        if (updated) {
            auto r = broadcastJobCompleteToAllClients(*updated);
            if (!r.isSuccess()) {
                warn("Failed to broadcast stage re-render completion: {}", r.getError()->getMessage());
            }
        }
    };
    auto updateProgress = [&](float v) {
        jobManager_->updateJobProgress(jobState.jobId, v);
        broadcastProgress(jobState.jobId);
    };
    auto failJob = [&](const std::string &msg) {
        error("Stage re-render job {} failed: {}", jobState.jobId, msg);
        if (jobState.span) {
            jobState.span->setError(msg);
        }
        jobManager_->failJob(jobState.jobId, msg);
        broadcastCompletion(jobState.jobId);
    };

    std::vector<std::string> animationIds;
    std::string requestedStageId;
    try {
        auto details = nlohmann::json::parse(jobState.details);
        if (details.contains("animation_ids") && details["animation_ids"].is_array()) {
            for (const auto &entry : details["animation_ids"]) {
                if (entry.is_string() && !entry.get<std::string>().empty()) {
                    animationIds.push_back(entry.get<std::string>());
                }
            }
        }
        requestedStageId = details.value("stage_id", std::string{});
    } catch (const std::exception &e) {
        return failJob(fmt::format("could not parse job details: {}", e.what()));
    }

    if (animationIds.empty()) {
        return failJob("no animation_ids to re-render");
    }
    if (jobState.span) {
        jobState.span->setAttribute("rerender.animation_count", static_cast<int64_t>(animationIds.size()));
        jobState.span->setAttribute("rerender.stage_id", requestedStageId);
    }

    std::size_t succeeded = 0;
    std::vector<std::string> failures;

    for (std::size_t index = 0; index < animationIds.size(); ++index) {
        const auto &animationId = animationIds[index];
        updateProgress(static_cast<float>(index) / static_cast<float>(animationIds.size()));

        auto animationResult = creatures::db->getAnimation(animationId, jobState.span);
        if (!animationResult.isSuccess()) {
            failures.push_back(fmt::format("{}: {}", animationId, animationResult.getError()->getMessage()));
            continue;
        }
        auto animation = animationResult.getValue().value();

        const auto msPerFrame = animation.metadata.milliseconds_per_frame;
        const auto totalFrames = static_cast<std::size_t>(animation.metadata.number_of_frames);
        if (msPerFrame == 0 || totalFrames == 0) {
            failures.push_back(fmt::format("{}: animation has no usable frame timing", animationId));
            continue;
        }

        // Which stage to aim against: the request's, or the one this animation
        // was originally rendered with.
        const std::string stageId = requestedStageId.empty() ? animation.metadata.source_stage_id : requestedStageId;
        if (stageId.empty()) {
            failures.push_back(fmt::format("{}: no stage to re-render against", animationId));
            continue;
        }
        auto stageResult = creatures::db->getStage(stageId, jobState.span);
        if (!stageResult.isSuccess()) {
            failures.push_back(
                fmt::format("{}: stage {}: {}", animationId, stageId, stageResult.getError()->getMessage()));
            continue;
        }
        const auto stage = stageResult.getValue().value();
        const auto placements = creatures::stagePlacements(stage);

        // Recover each creature's mouth bytes. Preferred source is the iXML
        // LIPSYNC block in the rendered WAV, which is authoritative; scraping
        // the existing track's mouth slot is the fallback for renders that
        // predate it. The distinction matters: a creature that froze on its
        // speech loop's first frame carries that frame's mouth value through
        // every silent frame, so scraping can mistake silence for speech if
        // the loop author left the beak open.
        std::unordered_map<std::string, std::vector<uint8_t>> mouthByCreature;
        std::string mouthSource = "scrape";

        if (!animation.metadata.sound_file.empty()) {
            // Map audio channel -> creature id once, so the per-lipsync-track
            // lookup below is a hash hit rather than a database round trip
            // inside a nested loop.
            std::unordered_map<uint16_t, std::string> creatureByChannel;
            for (const auto &track : animation.tracks) {
                if (track.creature_id.empty()) {
                    continue;
                }
                auto creatureResult = creatures::db->getCreature(track.creature_id, jobState.span);
                if (creatureResult.isSuccess() && creatureResult.getValue().has_value()) {
                    creatureByChannel.emplace(creatureResult.getValue().value().audio_channel, track.creature_id);
                }
            }

            const auto soundRoot = config->getSoundFileLocation();
            // resolveSoundInRoot takes a BARE FILENAME and searches for it
            // recursively; it rejects anything carrying a path component as a
            // traversal attempt. `sound_file` is stored with its subdirectory
            // ("dialog/scene-abc123.wav"), so handing it over unchanged always
            // returned nullopt and this whole iXML branch was dead — every
            // dialog re-render silently fell through to scraping the mouth
            // slot, which is the lossy path the design calls a last resort.
            const std::string soundBasename = std::filesystem::path(animation.metadata.sound_file).filename().string();
            if (auto resolved = creatures::audio::resolveSoundInRoot(soundRoot, soundBasename)) {
                if (auto ixml = voice::readIxmlChunk(*resolved)) {
                    const auto lipsyncTracks = voice::parseIxmlLipsync(*ixml);
                    creatures::SoundDataProcessor processor;
                    for (const auto &lipsync : lipsyncTracks) {
                        auto channelIt = creatureByChannel.find(lipsync.channel);
                        if (channelIt == creatureByChannel.end()) {
                            continue;
                        }
                        RhubarbSoundData snd;
                        snd.metadata.duration =
                            static_cast<double>(totalFrames) * static_cast<double>(msPerFrame) / 1000.0;
                        snd.metadata.soundFile = animation.metadata.sound_file;
                        snd.mouthCues.reserve(lipsync.cues.size());
                        for (const auto &cue : lipsync.cues) {
                            RhubarbMouthCue converted;
                            converted.start = cue.start;
                            converted.end = cue.end;
                            converted.value = cue.shape;
                            snd.mouthCues.push_back(converted);
                        }
                        mouthByCreature[channelIt->second] = processor.processSoundData(snd, msPerFrame, totalFrames);
                    }
                    if (!mouthByCreature.empty()) {
                        mouthSource = "ixml";
                    }
                }
            }
        }

        // Rebuild every track.
        std::mt19937 rng(static_cast<uint32_t>(animation.metadata.render_seed));

        // Resolve the geometry of everyone this stage places, so each creature
        // can aim at the others.
        std::vector<voice::GazeGeometry> geometries;
        std::unordered_map<std::string, std::size_t> geometryByCreature;
        for (const auto &placement : placements) {
            auto creatureResult = creatures::db->getCreature(placement.creature_id, jobState.span);
            if (!creatureResult.isSuccess() || !creatureResult.getValue().has_value()) {
                continue;
            }
            geometryByCreature.emplace(placement.creature_id, geometries.size());
            geometries.push_back(voice::resolveGazeGeometry(creatureResult.getValue().value(), placement));
        }

        creatures::Animation rebuilt = animation;
        rebuilt.tracks.clear();
        rebuilt.tracks.reserve(animation.tracks.size());

        // Fill any creature iXML didn't cover by scraping its existing track's
        // mouth slot. This has to happen BEFORE the timeline is built, not
        // lazily per track further down: the timeline is what tells every
        // creature who to look at, so a creature missing from it doesn't just
        // lose its own mouth bytes — it silently removes a speaker from the
        // scene. With iXML unavailable that left the timeline completely
        // empty, every creature holding its opening gaze for the whole
        // animation, and the job still reporting success.
        for (const auto &track : animation.tracks) {
            if (track.creature_id.empty() || mouthByCreature.count(track.creature_id)) {
                continue;
            }
            auto creatureResult = creatures::db->getCreature(track.creature_id, jobState.span);
            if (!creatureResult.isSuccess() || !creatureResult.getValue().has_value()) {
                continue;
            }
            const std::size_t slot = creatures::resolvedMouthSlot(creatureResult.getValue().value());
            std::vector<uint8_t> scraped;
            scraped.reserve(track.frames.size());
            for (const auto &encoded : track.frames) {
                const auto decoded = decodeBase64(encoded);
                scraped.push_back(slot < decoded.size() ? decoded[slot] : 0);
            }
            mouthByCreature[track.creature_id] = std::move(scraped);
        }

        // The speaker timeline, from the recovered mouth bytes.
        std::vector<std::string> timelineIds;
        std::vector<std::span<const uint8_t>> timelineMouths;
        std::vector<std::vector<uint8_t>> mouthStorage;
        mouthStorage.reserve(animation.tracks.size());
        for (const auto &track : animation.tracks) {
            if (track.creature_id.empty()) {
                continue;
            }
            auto it = mouthByCreature.find(track.creature_id);
            if (it == mouthByCreature.end()) {
                continue;
            }
            mouthStorage.push_back(it->second);
            timelineIds.push_back(track.creature_id);
        }
        for (const auto &stored : mouthStorage) {
            timelineMouths.emplace_back(stored);
        }

        // An empty timeline means nobody is recorded as speaking, so no
        // creature would ever re-aim. That's an unusable re-render, not a
        // quiet no-op — fail loudly rather than write a scene where everyone
        // stares straight ahead.

        const std::size_t gapTolerance = std::max<std::size_t>(1, 400 / std::max<uint32_t>(1, msPerFrame));
        const auto timeline = voice::buildSpeakerTimeline(timelineIds, timelineMouths, totalFrames, gapTolerance);

        bool trackFailure = false;
        for (const auto &track : animation.tracks) {
            // Fixture tracks and anything without a creature pass through
            // untouched — this job only rebuilds creature motion.
            if (track.creature_id.empty()) {
                rebuilt.tracks.push_back(track);
                continue;
            }

            auto creatureResult = creatures::db->getCreature(track.creature_id, jobState.span);
            if (!creatureResult.isSuccess() || !creatureResult.getValue().has_value()) {
                failures.push_back(fmt::format("{}: creature {} not found", animationId, track.creature_id));
                trackFailure = true;
                break;
            }
            const auto creature = creatureResult.getValue().value();

            // Replay the recorded loop choices so the body motion is
            // reproduced rather than re-drawn.
            const auto choiceIt = std::find_if(
                animation.metadata.source_render_choices.begin(), animation.metadata.source_render_choices.end(),
                [&](const creatures::CreatureRenderChoice &c) { return c.creature_id == track.creature_id; });
            if (choiceIt == animation.metadata.source_render_choices.end()) {
                failures.push_back(fmt::format(
                    "{}: no recorded render choices for creature {} — this animation predates them and can't be "
                    "re-rendered without changing its body motion",
                    animationId, track.creature_id));
                trackFailure = true;
                break;
            }

            auto loadLoopFrames = [&](const std::string &loopAnimationId,
                                      std::vector<std::vector<uint8_t>> &out) -> std::string {
                if (loopAnimationId.empty()) {
                    return {};
                }
                auto loopResult = creatures::db->getAnimation(loopAnimationId, jobState.span);
                if (!loopResult.isSuccess()) {
                    return loopResult.getError()->getMessage();
                }
                const auto loopAnimation = loopResult.getValue().value();
                auto loopTrack = std::find_if(loopAnimation.tracks.begin(), loopAnimation.tracks.end(),
                                              [&](const Track &t) { return t.creature_id == track.creature_id; });
                if (loopTrack == loopAnimation.tracks.end()) {
                    return fmt::format("animation {} has no track for this creature", loopAnimationId);
                }
                for (const auto &frame : loopTrack->frames) {
                    out.push_back(decodeBase64(frame));
                }
                return {};
            };

            std::vector<std::vector<uint8_t>> baseFrames;
            if (auto err = loadLoopFrames(choiceIt->speech_loop_animation_id, baseFrames); !err.empty()) {
                failures.push_back(fmt::format("{}: speech loop for {}: {}", animationId, track.creature_id, err));
                trackFailure = true;
                break;
            }
            if (baseFrames.empty()) {
                failures.push_back(
                    fmt::format("{}: speech loop for {} decoded to zero frames", animationId, track.creature_id));
                trackFailure = true;
                break;
            }
            std::vector<std::vector<uint8_t>> idleFrames;
            if (auto err = loadLoopFrames(choiceIt->idle_animation_id, idleFrames); !err.empty()) {
                // Non-fatal, exactly as at first render: fall back to freezing.
                warn("Stage re-render {}: idle loop for {} unavailable ({}); freezing during silence", animationId,
                     track.creature_id, err);
                idleFrames.clear();
            }

            // Recovered above, for every creature, before the timeline was
            // built — so this is always a hit.
            const std::size_t mouthSlot = creatures::resolvedMouthSlot(creature);
            const std::vector<uint8_t> mouthBytes = mouthByCreature[track.creature_id];

            // Gaze against the NEW stage.
            std::mt19937 gazeRng(static_cast<uint32_t>(rng()));
            voice::GazeTrack gaze;
            if (auto it = geometryByCreature.find(track.creature_id); it != geometryByCreature.end()) {
                gaze = voice::buildGazeTrack(geometries[it->second], geometries, timeline, totalFrames, msPerFrame,
                                             gazeRng);
            }

            voice::SpeechTrackInput trackInput;
            trackInput.baseFrames = baseFrames;
            trackInput.mouthBytes = mouthBytes;
            trackInput.mouthSlot = mouthSlot;
            trackInput.totalFrames = totalFrames;
            trackInput.creatureId = track.creature_id;
            trackInput.animationId = animation.id;
            trackInput.gazePanBytes = gaze.panBytes;
            trackInput.gazeElevationBytes = gaze.elevationBytes;
            trackInput.gazeCockBytes = gaze.cockBytes;
            trackInput.gazePanSlot = gaze.panSlot;
            trackInput.gazeElevationSlot = gaze.elevationSlot;
            trackInput.gazeCockSlot = gaze.cockSlot;

            voice::SpeechTrackOptions trackOptions;
            trackOptions.dialogIdleMode = true;
            trackOptions.bodyTailFrames = 5;
            trackOptions.idleFrames = idleFrames;
            trackOptions.idleStartOffset = choiceIt->idle_start_offset;

            auto trackResult = voice::buildSpeechTrack(trackInput, trackOptions, jobState.span);
            if (!trackResult.isSuccess()) {
                failures.push_back(fmt::format("{}: {}", animationId, trackResult.getError()->getMessage()));
                trackFailure = true;
                break;
            }
            auto newTrack = trackResult.getValue()->track;
            // Keep the track's identity — this is the same track, re-motioned.
            newTrack.id = track.id;
            rebuilt.tracks.push_back(std::move(newTrack));
        }

        if (trackFailure) {
            continue;
        }

        // Re-stamp the stage provenance. The sound file, script provenance,
        // seed and render choices all carry over untouched — that's the point.
        rebuilt.metadata.source_stage_id = stage.id;
        rebuilt.metadata.source_stage_updated_at = stage.updated_at;

        auto published =
            creatures::storage::publishAnimation(creatures::animationToJson(rebuilt).dump(), jobState.span);
        if (!published.isSuccess()) {
            failures.push_back(fmt::format("{}: {}", animationId, published.getError()->getMessage()));
            continue;
        }

        info("Stage re-render {}: rebuilt animation {} against stage '{}' ({} tracks, mouth source: {})",
             jobState.jobId, animationId, stage.title, rebuilt.tracks.size(), mouthSource);
        ++succeeded;
    }

    updateProgress(1.0f);

    nlohmann::json result;
    result["rerendered"] = succeeded;
    result["requested"] = animationIds.size();
    result["failures"] = failures;
    if (jobState.span) {
        jobState.span->setAttribute("rerender.succeeded", static_cast<int64_t>(succeeded));
        jobState.span->setAttribute("rerender.failed", static_cast<int64_t>(failures.size()));
    }

    if (succeeded == 0 && !failures.empty()) {
        return failJob(fmt::format("no animations could be re-rendered: {}", failures.front()));
    }
    jobManager_->completeJob(jobState.jobId, result.dump());
    broadcastCompletion(jobState.jobId);
}

void JobWorker::handleDialogJob(JobState &jobState) {
    auto broadcastProgress = [this](const std::string &jobId) {
        auto updated = jobManager_->getJob(jobId);
        if (updated) {
            auto r = broadcastJobProgressToAllClients(*updated);
            if (!r.isSuccess()) {
                warn("Failed to broadcast dialog job progress: {}", r.getError()->getMessage());
            }
        }
    };
    auto broadcastCompletion = [this](const std::string &jobId) {
        auto updated = jobManager_->getJob(jobId);
        if (updated) {
            auto r = broadcastJobCompleteToAllClients(*updated);
            if (!r.isSuccess()) {
                warn("Failed to broadcast dialog job completion: {}", r.getError()->getMessage());
            }
        }
    };
    auto updateProgress = [&](float v) {
        jobManager_->updateJobProgress(jobState.jobId, v);
        broadcastProgress(jobState.jobId);
    };
    auto failJob = [&](const std::string &msg) {
        error("Dialog job {} failed: {}", jobState.jobId, msg);
        if (jobState.span) {
            jobState.span->setError(msg);
        }
        jobManager_->failJob(jobState.jobId, msg);
        broadcastCompletion(jobState.jobId);
    };

    // ---- Parse the job details — which is the controller's serialized
    // DialogRequestDto. Round-trip through oatpp's ObjectMapper so the schema
    // is enforced on both ends rather than picked apart by hand.
    auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
    oatpp::Object<ws::DialogRequestDto> reqDto;
    try {
        reqDto = jsonMapper->readFromString<oatpp::Object<ws::DialogRequestDto>>(jobState.details.c_str());
    } catch (const std::exception &e) {
        return failJob(fmt::format("invalid dialog job details: {}", e.what()));
    }
    if (!reqDto) {
        return failJob("dialog job details deserialized to null");
    }
    const bool hasInlineTurns = reqDto->turns && !reqDto->turns->empty();
    const bool hasScriptId = reqDto->script_id && !reqDto->script_id->empty();
    if (hasInlineTurns && hasScriptId) {
        return failJob("dialog job got both turns[] and script_id — controller should have rejected this");
    }
    if (!hasInlineTurns && !hasScriptId) {
        return failJob("dialog job requires turns[] or script_id");
    }
    if (!reqDto->persistence) {
        return failJob("dialog job requires persistence ('adhoc' or 'permanent')");
    }

    std::vector<std::pair<std::string, std::string>> rawTurns;
    // Provenance for the rendered Animation. Empty when rendering from inline
    // turns (no script to point at); populated when loading from a script.
    std::string sourceScriptId;
    std::string scriptStageId;             // the script's own stage binding, if it has one (#128)
    std::string scriptTitle;               // the script's own title, used when the request doesn't give one
    std::string acceptedVoiceGenerationId; // the script's accepted take, if any (#131)
    std::vector<creatures::DialogScriptTurn> sourceScriptTurns;
    std::optional<creatures::DialogBackgroundMusic> backgroundMusic;
    if (hasScriptId) {
        // Re-validate at the trust boundary even though the controller already did —
        // the worker rehydrates from a serialized blob and may eventually be fed
        // from non-controller sources (retry, cron). Keeps attacker strings out of
        // log lines and span attributes (security review S2).
        const std::string requestedScriptId(*reqDto->script_id);
        if (!isUuidShape(requestedScriptId)) {
            return failJob("script_id is not a UUID");
        }
        auto sr = creatures::db->getDialogScript(requestedScriptId, jobState.span);
        if (!sr.isSuccess()) {
            return failJob(fmt::format("script_id lookup failed: {}", sr.getError().value().getMessage()));
        }
        const auto script = sr.getValue().value();
        if (script.turns.empty()) {
            return failJob(fmt::format("script_id '{}' has no turns", script.id));
        }
        rawTurns.reserve(script.turns.size());
        for (const auto &t : script.turns) {
            rawTurns.emplace_back(t.creature_id, t.text);
        }
        sourceScriptId = script.id;
        sourceScriptTurns = script.turns;
        backgroundMusic = script.background_music;
        scriptStageId = script.stage_id;
        scriptTitle = script.title;
        if (script.accepted_voice) {
            acceptedVoiceGenerationId = script.accepted_voice->generation_id;
        }
    } else {
        rawTurns.reserve(reqDto->turns->size());
        sourceScriptTurns.reserve(reqDto->turns->size());
        for (const auto &t : *reqDto->turns) {
            if (!t || !t->creature_id || !t->text) {
                return failJob("each turn must have a non-null creature_id and text");
            }
            rawTurns.emplace_back(*t->creature_id, *t->text);
            // Snapshot the inline turns for provenance too (#60). There's no saved
            // script to point at (sourceScriptId stays empty), but the CoW snapshot
            // still records exactly what was rendered so the document keeps its history.
            sourceScriptTurns.push_back(creatures::DialogScriptTurn{*t->creature_id, *t->text});
        }
    }

    DialogPersistence persistence = DialogPersistence::AdHoc;
    {
        const std::string pstr = *reqDto->persistence;
        if (pstr == "adhoc") {
            persistence = DialogPersistence::AdHoc;
        } else if (pstr == "permanent") {
            persistence = DialogPersistence::Permanent;
        } else {
            return failJob(fmt::format("unknown persistence '{}' (expected 'adhoc' or 'permanent')", pstr));
        }
    }
    const bool autoplay = reqDto->autoplay ? *reqDto->autoplay : false;
    std::string title = reqDto->title ? std::string(*reqDto->title) : std::string{};
    const std::string requestedGenerationId =
        reqDto->generation_id ? std::string(*reqDto->generation_id) : std::string{};
    // Stage binding (#119). Three cases, and the middle one is the ordinary
    // render (#128):
    //
    //   request sets an id  -> use it (this is how you render a travel
    //                          version of a mainstage scene)
    //   request omits it    -> inherit the script's own stage_id, which is
    //                          the entire reason DialogScript carries one
    //   request sends ""    -> force no stage, so the override works in both
    //                          directions rather than only toward "more"
    std::string stageId;
    if (reqDto->stage_id) {
        stageId = std::string(*reqDto->stage_id); // may be "" — deliberate opt-out
    } else {
        stageId = scriptStageId;
        if (!stageId.empty()) {
            debug("Dialog job {}: inheriting stage {} from script {}", jobState.jobId, stageId, sourceScriptId);
        }
    }
    // Title precedence: what the request asked for, else the script's own
    // title, else a last-resort job id.
    //
    // The script fallback matters more than it looks. Rendering from a saved
    // script without an explicit title is the ORDINARY case, and falling
    // straight to "Dialog <uuid>" put a UUID back into the title — and
    // therefore straight back into the audio filename, undoing #126 for the
    // most common path.
    if (title.empty()) {
        title = scriptTitle;
    }
    if (title.empty()) {
        title = fmt::format("Dialog {}", jobState.jobId);
    }

    // Resolve the stage up front rather than at the point head aiming needs
    // it, because the title depends on it and the audio filename depends on
    // the title (#126). A mainstage and a travel rendition of one script
    // otherwise produce identical titles AND identical-looking filenames.
    creatures::Stage renderStage;
    bool haveStage = false;
    if (!stageId.empty()) {
        auto stageResult = creatures::db->getStage(stageId, jobState.span);
        if (!stageResult.isSuccess()) {
            // A dangling stage id shouldn't cost you the whole render — the
            // dialog is expensive and the head aiming is a garnish.
            warn("Dialog job {}: stage {} could not be loaded ({}); rendering without head aiming", jobState.jobId,
                 stageId, stageResult.getError().value().getMessage());
        } else {
            renderStage = stageResult.getValue().value();
            haveStage = true;
            // Only append if it isn't already there. A re-render often
            // submits the previous animation's title back, which already
            // carries the suffix — appending unconditionally produced
            // "Scene 3 — Travel — Travel", and one more each time.
            if (!renderStage.title.empty()) {
                const auto suffix = fmt::format(" — {}", renderStage.title);
                if (title.size() < suffix.size() ||
                    title.compare(title.size() - suffix.size(), suffix.size(), suffix) != 0) {
                    title += suffix;
                }
            }
        }
    }

    if (jobState.span) {
        jobState.span->setAttribute("dialog.turns", static_cast<int64_t>(rawTurns.size()));
        jobState.span->setAttribute("dialog.persistence",
                                    persistence == DialogPersistence::AdHoc ? "adhoc" : "permanent");
        jobState.span->setAttribute("dialog.autoplay", autoplay);
        if (!sourceScriptId.empty()) {
            jobState.span->setAttribute("dialog.script_id", sourceScriptId);
            jobState.span->setAttribute("dialog.script_turns", static_cast<int64_t>(sourceScriptTurns.size()));
        }
    }

    // ---- Resolve every UNIQUE creature in turns. Domain validation runs here
    // (the controller only checks well-formedness of the DTO).
    std::vector<DialogJobCreature> creaturesCache;
    std::unordered_map<std::string, std::size_t> byCreatureId;
    for (const auto &[cid, _text] : rawTurns) {
        if (cid.empty()) {
            return failJob("a turn has empty creature_id");
        }
        // Pre-filter through isUuidShape so non-UUID creature_id (which can never
        // resolve anyway) doesn't reach DB queries / log lines / span attributes
        // unsanitized (security review S4).
        if (!isUuidShape(cid)) {
            return failJob("a turn has a creature_id that is not a UUID");
        }
        if (byCreatureId.count(cid)) {
            continue;
        }
        auto jr = creatures::db->getCreatureJson(cid, jobState.span);
        if (!jr.isSuccess()) {
            return failJob(fmt::format("creature '{}' lookup failed: {}", cid, jr.getError().value().getMessage()));
        }
        const auto cj = jr.getValue().value();
        if (!cj.contains("voice") || !cj["voice"].is_object() || !cj["voice"].contains("voice_id") ||
            !cj["voice"]["voice_id"].is_string()) {
            return failJob(fmt::format("creature '{}' has no voice.voice_id", cid));
        }
        if (!cj.contains("audio_channel") || !cj["audio_channel"].is_number()) {
            return failJob(fmt::format("creature '{}' has no audio_channel", cid));
        }
        if (!cj.contains("mouth_slot") || !cj["mouth_slot"].is_number()) {
            return failJob(fmt::format("creature '{}' has no mouth_slot", cid));
        }
        DialogJobCreature c;
        c.creatureId = cid;
        c.creatureJson = cj;
        c.voiceId = cj["voice"]["voice_id"].get<std::string>();
        c.audioChannel = cj["audio_channel"].get<uint16_t>();
        c.mouthSlot = cj["mouth_slot"].get<uint8_t>();
        c.universe = 0;
        try {
            auto u = creatures::creatureUniverseMap->get(cid);
            if (u) {
                c.universe = *u;
            }
        } catch (...) {
            // Not registered — fine unless autoplay is set; checked below.
        }
        byCreatureId.emplace(cid, creaturesCache.size());
        creaturesCache.push_back(std::move(c));
    }

    // Cross-creature checks.
    //
    // audio_channel MUST be distinct — the 17-channel WAV interleave writes
    // each creature's PCM into its lane; two creatures sharing a lane would
    // silently clobber one. Caught here AND in writeDialogWav as belt+braces.
    //
    // mouth_slot does NOT need to be distinct: it's a per-creature local byte
    // offset into THAT creature's Track frames. Each creature gets its own
    // Track in the multi-track Animation, and each creature's controller
    // reads only its own track — so two creatures both having mouth_slot=4
    // means each writes byte 4 of its OWN frame buffer to its OWN mouth
    // servo. No collision. (Beaky and Mango both have mouth_slot=4 in
    // their stored configs.)
    std::unordered_set<uint16_t> seenChannels;
    std::unordered_set<std::string> uniqueVoices;
    for (const auto &c : creaturesCache) {
        if (!seenChannels.insert(c.audioChannel).second) {
            return failJob(
                fmt::format("audio_channel {} is assigned to more than one creature in this scene", c.audioChannel));
        }
        uniqueVoices.insert(c.voiceId);
    }
    if (uniqueVoices.size() > kMaxUniqueVoicesPerScene) {
        return failJob(
            fmt::format("{} unique voices exceeds per-scene cap of {}", uniqueVoices.size(), kMaxUniqueVoicesPerScene));
    }
    if (autoplay) {
        std::vector<creatureId_t> autoplayCreatures;
        autoplayCreatures.reserve(creaturesCache.size());
        for (const auto &c : creaturesCache) {
            autoplayCreatures.push_back(c.creatureId);
        }
        auto universeResult = creatures::resolveCommonUniverse(autoplayCreatures);
        if (!universeResult.isSuccess()) {
            return failJob(fmt::format("autoplay requested but {}", universeResult.getError().value().getMessage()));
        }
    }
    if (jobState.span) {
        jobState.span->setAttribute("dialog.unique_creatures", static_cast<int64_t>(creaturesCache.size()));
        jobState.span->setAttribute("dialog.unique_voices", static_cast<int64_t>(uniqueVoices.size()));
    }
    updateProgress(0.05f);

    // ---- Build DialogInput list (one per turn, looking up voice_id per
    // creature) and chunk.
    std::vector<voice::DialogInput> inputs;
    inputs.reserve(rawTurns.size());
    for (const auto &[cid, text] : rawTurns) {
        const auto &c = creaturesCache[byCreatureId.at(cid)];
        inputs.push_back({c.voiceId, text});
    }
    // ---- Accepted voice take (#131) ---------------------------------------
    // When the script has one and the request didn't override it, render from
    // that exact take. The controller already refused the request if the
    // acceptance was missing or stale, so reaching here means it's good.
    //
    // The take is a WHOLE-SCENE generation — the preview assembles all chunks
    // and saves the merged result under computeCacheKey(all inputs). So skip
    // chunking entirely and treat the scene as one chunk: the per-chunk cache
    // key then equals the whole-scene key by construction, and the existing
    // explicit-generation_id path picks it up unchanged.
    //
    // Without this, a multi-chunk script could be accepted and never rendered
    // — the render would look for the take in per-chunk caches it was never
    // stored in. That would make a long scene permanently unrenderable under
    // the strict gate.
    std::string effectiveGenerationId = requestedGenerationId;
    bool renderingAcceptedTake = false;
    if (effectiveGenerationId.empty() && !acceptedVoiceGenerationId.empty()) {
        effectiveGenerationId = acceptedVoiceGenerationId;
        renderingAcceptedTake = true;
    }

    std::vector<std::vector<voice::DialogInput>> chunks;
    if (renderingAcceptedTake) {
        chunks.push_back(inputs);
        info("Dialog job {}: rendering the script's accepted take {} as a single whole-scene chunk", jobState.jobId,
             effectiveGenerationId);
    } else {
        auto chunksResult = voice::chunkTurns(inputs);
        if (!chunksResult.isSuccess()) {
            return failJob(chunksResult.getError().value().getMessage());
        }
        chunks = chunksResult.getValue().value();
    }
    if (jobState.span) {
        jobState.span->setAttribute("dialog.chunks", static_cast<int64_t>(chunks.size()));
        jobState.span->setAttribute("dialog.rendering_accepted_take", renderingAcceptedTake);
    }

    // ---- Per-chunk: text-to-dialogue + forced-alignment + assemble.
    //
    // Progress: we reserve 0.10..0.55 for these (each chunk gets an equal
    // slice). On a single-chunk scene (the common case) the bar moves smoothly.
    voice::DialogClient client;
    const std::string apiKey = creatures::config->getVoiceApiKey();

    std::vector<voice::DialogAssembled> assembledChunks;
    assembledChunks.reserve(chunks.size());
    // The ElevenLabs generation id actually used for each chunk (cache hit or
    // fresh), for the WAV's embedded provenance (#47).
    std::vector<std::string> generationIds;
    generationIds.reserve(chunks.size());
    for (std::size_t ci = 0; ci < chunks.size(); ++ci) {
        const auto &chunk = chunks[ci];
        std::string chunkGenerationId;
        auto chunkSpan =
            creatures::observability->createChildOperationSpan(fmt::format("DialogJob.chunk.{}", ci), jobState.span);

        // ---- Cache lookup, before paying for ElevenLabs.
        // Resolution order:
        //   1. If the request named a specific generation_id, look ONLY for
        //      that. Stale (cron-cleaned) → log + fall through to fresh.
        //   2. Else, return the latest cached generation matching this chunk's
        //      turns, if any.
        //   3. Else, call ElevenLabs and save the result for next time.
        // The cache key is per-CHUNK. Multi-chunk scenes get per-chunk benefit
        // (a chunk with identical text/voices to a prior run hits the cache
        // even if the surrounding chunks differ).
        const auto cacheKey = voice::computeCacheKey(chunk);
        if (chunkSpan) {
            chunkSpan->setAttribute("dialog.cache_key", cacheKey);
        }

        std::vector<uint8_t> chunkAudio;
        std::vector<voice::DialogVoiceSegment> chunkSegments;
        voice::ForcedAlignmentResult chunkAlignment;
        bool cacheHit = false;
        bool segmentNormalizationApplied = false;

        // Only honor the explicit generation_id on a SINGLE-chunk scene —
        // for multi-chunk scenes the id would only match one chunk's cache
        // anyway, and we don't want to confuse the user about which chunk
        // got reused.
        const bool useExplicitId = !effectiveGenerationId.empty() && chunks.size() == 1;
        if (useExplicitId) {
            // An ACCEPTED take lives in the durable store; look there first.
            // The ephemeral cache is only ever an optimisation (issue #146).
            auto loadResult = renderingAcceptedTake
                                  ? voice::loadAcceptedGeneration(cacheKey, effectiveGenerationId)
                                  : Result<voice::CachedGeneration>{ServerError(ServerError::NotFound, "not accepted")};
            if (!loadResult.isSuccess()) {
                loadResult = voice::loadGeneration(cacheKey, effectiveGenerationId);
            }
            if (!loadResult.isSuccess() && renderingAcceptedTake) {
                // Never regenerate behind the user's back. They auditioned and
                // accepted a specific performance; silently producing a
                // different one is the exact failure the accepted-take feature
                // exists to prevent, and it costs money doing it.
                return failJob(fmt::format(
                    "the script's accepted voice take {} could not be loaded ({}). Refusing to regenerate audio, "
                    "which would produce a different performance — re-accept a take for this script.",
                    effectiveGenerationId, loadResult.getError().value().getMessage()));
            }
            if (loadResult.isSuccess()) {
                auto gen = loadResult.getValue().value();
                segmentNormalizationApplied = voice::normalizeCachedGenerationVoiceSegments(gen, chunk);
                chunkGenerationId = gen.generationId;
                chunkAudio = std::move(gen.audioPcm);
                chunkSegments = std::move(gen.voiceSegments);
                chunkAlignment = std::move(gen.forcedAlignment);
                cacheHit = true;
                info("Dialog job {}: chunk {} using requested generation_id={}", jobState.jobId, ci,
                     effectiveGenerationId);
            } else {
                warn("Dialog job {}: requested generation_id={} not in cache for this chunk — regenerating",
                     jobState.jobId, effectiveGenerationId);
            }
        }
        if (!cacheHit) {
            if (auto latest = voice::findLatestGeneration(cacheKey)) {
                auto loadResult = voice::loadGeneration(cacheKey, *latest);
                if (loadResult.isSuccess()) {
                    auto gen = loadResult.getValue().value();
                    segmentNormalizationApplied = voice::normalizeCachedGenerationVoiceSegments(gen, chunk);
                    chunkGenerationId = gen.generationId;
                    chunkAudio = std::move(gen.audioPcm);
                    chunkSegments = std::move(gen.voiceSegments);
                    chunkAlignment = std::move(gen.forcedAlignment);
                    cacheHit = true;
                    info("Dialog job {}: chunk {} reusing latest cached generation_id={}", jobState.jobId, ci, *latest);
                }
            }
        }

        if (!cacheHit) {
            // Shared generate → align → cache block (also used by the preview paths).
            auto genResult = voice::generateChunkWithAlignment(client, apiKey, chunk, cacheKey, chunkSpan);
            if (!genResult.isSuccess()) {
                return failJob(fmt::format("chunk {}: {}", ci, genResult.getError().value().getMessage()));
            }
            auto gen = genResult.getValue().value();
            chunkGenerationId = gen.generationId;
            chunkAudio = std::move(gen.audioPcm);
            chunkSegments = std::move(gen.voiceSegments);
            chunkAlignment = std::move(gen.forcedAlignment);
            segmentNormalizationApplied = true;
        }
        if (chunkSpan) {
            chunkSpan->setAttribute("dialog.cache_hit", cacheHit);
            chunkSpan->setAttribute("dialog.segment_index_space", voice::kVoiceSegmentIndexSpaceNormalized);
            chunkSpan->setAttribute("dialog.segment_normalization_applied", segmentNormalizationApplied);
        }
        generationIds.push_back(chunkGenerationId);

        // Reassemble the DialogResult shape that assembleChunk expects.
        voice::DialogResult dialog;
        dialog.audioData = std::move(chunkAudio);
        dialog.audioFormat = "pcm_48000";
        dialog.voiceSegments = std::move(chunkSegments);

        auto assembleResult = voice::assembleChunk(chunk, dialog, chunkAlignment, kDialogSampleRate);
        if (!assembleResult.isSuccess()) {
            if (chunkSpan) {
                recordSpanError(chunkSpan, assembleResult.getError().value().getMessage(), "DialogAssemblyError",
                                assembleResult.getError().value().getCode());
                chunkSpan->setAttribute("dialog.assembly_failed", true);
            }
            return failJob(
                fmt::format("chunk {} assembleChunk: {}", ci, assembleResult.getError().value().getMessage()));
        }
        assembledChunks.push_back(assembleResult.getValue().value());

        // Linear progress across chunks within 0.10..0.55.
        const float frac = static_cast<float>(ci + 1) / static_cast<float>(chunks.size());
        updateProgress(0.10f + 0.45f * frac);
    }

    auto concatResult = voice::concatChunks(assembledChunks);
    if (!concatResult.isSuccess()) {
        return failJob(concatResult.getError().value().getMessage());
    }
    const auto assembled = concatResult.getValue().value();
    updateProgress(0.60f);

    std::vector<int16_t> backgroundMusicSamples;
    std::optional<voice::MusicWavProvenance> backgroundMusicProvenance;
    if (backgroundMusic) {
        auto musicSpan =
            creatures::observability->createChildOperationSpan("DialogJob.loadBackgroundMusic", jobState.span);
        if (musicSpan) {
            musicSpan->setAttribute("music.generation_id", backgroundMusic->generation_id);
            musicSpan->setAttribute("sound.file_hash", util::sha256Hex(backgroundMusic->sound_file));
            musicSpan->setAttribute("sound.file_extension",
                                    std::filesystem::path(backgroundMusic->sound_file).extension().string());
        }
        const auto failMusicLoad = [&](const std::string &message, const std::string &type,
                                       ServerError::Code code = ServerError::InvalidData) {
            recordSpanError(musicSpan, message, type, code);
            failJob(message);
        };
        const auto musicPath = storage::resolveSoundPath(backgroundMusic->sound_file);
        const auto maxMusicSamples =
            static_cast<std::uint64_t>(voice::kMaxMusicLengthMs) * static_cast<std::uint64_t>(kDialogSampleRate) / 1000;
        auto musicResult = audio::loadWavAsMono(musicPath.string(), maxMusicSamples);
        if (!musicResult.isSuccess()) {
            return failMusicLoad(fmt::format("accepted background music could not be loaded: {}",
                                             musicResult.getError().value().getMessage()),
                                 "AudioLoadError", musicResult.getError().value().getCode());
        }
        const auto music = musicResult.getValue().value();
        if (music.sampleRate != kDialogSampleRate) {
            return failMusicLoad(fmt::format("accepted background music sample rate {} is not {} Hz", music.sampleRate,
                                             kDialogSampleRate),
                                 "InvalidAudioFormat");
        }
        backgroundMusicSamples = music.samples;
        const auto ixml = voice::readIxmlChunk(musicPath);
        if (!ixml) {
            return failMusicLoad("accepted background music has no embedded provenance", "MissingProvenance");
        }
        const auto musicProvenance = voice::parseIxmlProvenance(*ixml);
        if (!musicProvenance.music || musicProvenance.music->musicGenerationId != backgroundMusic->generation_id) {
            return failMusicLoad("accepted background music provenance does not match the dialog reference",
                                 "ProvenanceVerificationError");
        }
        const auto musicBytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(music.samples.data()),
                                                         music.samples.size() * sizeof(int16_t));
        if (musicProvenance.music->pcmSha256.empty() ||
            util::sha256Hex(musicBytes) != musicProvenance.music->pcmSha256) {
            return failMusicLoad("accepted background music PCM checksum does not match its provenance",
                                 "ChecksumMismatch");
        }
        backgroundMusicProvenance = *musicProvenance.music;
        if (musicSpan) {
            musicSpan->setAttribute("audio.sample_rate", static_cast<int64_t>(music.sampleRate));
            musicSpan->setAttribute("audio.samples", static_cast<int64_t>(music.samples.size()));
            musicSpan->setAttribute("music.checksum_verified", true);
            musicSpan->setSuccess();
        }
    }

    const auto showTimelineSamples = voice::dialogPlaybackSampleCount(assembled, backgroundMusicSamples);
    const auto musicTailSamples = showTimelineSamples - assembled.totalSamples;
    if (jobState.span) {
        const auto samplesToMs = [](std::size_t samples) {
            return static_cast<int64_t>(samples * 1000 / kDialogSampleRate);
        };
        jobState.span->setAttribute("dialog.speech_samples", static_cast<int64_t>(assembled.totalSamples));
        jobState.span->setAttribute("dialog.show_samples", static_cast<int64_t>(showTimelineSamples));
        jobState.span->setAttribute("music.tail_samples", static_cast<int64_t>(musicTailSamples));
        jobState.span->setAttribute("dialog.speech_duration_ms", samplesToMs(assembled.totalSamples));
        jobState.span->setAttribute("dialog.show_duration_ms", samplesToMs(showTimelineSamples));
        jobState.span->setAttribute("music.tail_duration_ms", samplesToMs(musicTailSamples));
        jobState.span->setAttribute("music.extends_show", musicTailSamples > 0);
    }

    // ---- 17-channel WAV output. The storage facade owns the path math AND
    // the absolute-vs-relative metadata convention (Permanent stores relative,
    // AdHoc stores absolute) so this handler doesn't reinvent it.
    const auto wavBucket = persistence == DialogPersistence::AdHoc ? creatures::storage::Persistence::AdHoc
                                                                   : creatures::storage::Persistence::Permanent;
    // Name the audio after what it IS, not after the job that made it (#126).
    // A UUID is the only label this file carries once it leaves the system —
    // in the sound store, the Console's sound list, and most importantly the
    // shared MP3 rendition, which derives its name from this basename.
    //
    // `title` already carries the stage suffix, so a mainstage and a travel
    // rendition of one scene get distinguishable names. The short job-id tail
    // keeps re-renders and identically-titled scripts from colliding.
    const auto exportName = util::exportBasename(title, jobState.jobId);
    const auto wavFilename = persistence == DialogPersistence::AdHoc ? fmt::format("dialog_{}.wav", exportName)
                                                                     : fmt::format("{}.wav", exportName);
    std::optional<std::string> wavSubdir;
    if (persistence == DialogPersistence::Permanent) {
        wavSubdir = std::string(kPermanentDialogSubdir);
    }
    auto wavPathResult = creatures::storage::allocateSoundPath(wavBucket, wavFilename, wavSubdir);
    if (!wavPathResult.isSuccess()) {
        return failJob(fmt::format("allocateSoundPath: {}", wavPathResult.getError()->getMessage()));
    }
    const auto wavStoragePath = wavPathResult.getValue().value();
    const auto wavPath = wavStoragePath.absolute;
    const auto animationSoundFile = wavStoragePath.forMetadata;

    voice::VoiceChannelMap voiceToChannel;
    for (const auto &c : creaturesCache) {
        voiceToChannel.emplace(c.voiceId, c.audioChannel);
    }

    // Build embedded provenance for permanent renders so an otherwise anonymous
    // dialog/<uuid>.wav can be traced back to its script (#47). Ad-hoc and
    // preview WAVs stay lean (nullptr below). This is a point-in-time snapshot,
    // mirroring animation.metadata.source_script_turns.
    voice::WavProvenance provenance;
    if (persistence == DialogPersistence::Permanent) {
        provenance.fileUid = jobState.jobId;
        provenance.take = jobState.jobId;
        provenance.circled = true;
        provenance.sourceScriptId = sourceScriptId;
        provenance.title = title;
        provenance.generationIds = generationIds;
        provenance.music = backgroundMusicProvenance;

        // creature_id → display name for the track list and speaker labels.
        std::unordered_map<std::string, std::string> nameById;
        for (const auto &c : creaturesCache) {
            const std::string name = c.creatureJson.value("name", std::string{});
            nameById.emplace(c.creatureId, name.empty() ? c.creatureId : name);
        }

        // Track list: each creature on its 1-based audio channel (sorted), plus
        // the reserved BGM lane. Creatures validate to 1..16, so 17 is always BGM.
        for (const auto &c : creaturesCache) {
            provenance.tracks.push_back({c.audioChannel, nameById.at(c.creatureId)});
        }
        std::sort(
            provenance.tracks.begin(), provenance.tracks.end(),
            [](const voice::DialogTrackInfo &a, const voice::DialogTrackInfo &b) { return a.channel < b.channel; });
        provenance.tracks.push_back({static_cast<uint16_t>(RTP_STREAMING_CHANNELS), "BGM"});

        // Script snapshot: rawTurns is exactly what was rendered (a copy of the
        // saved script's turns when rendering from one), resolved to names.
        provenance.script.reserve(rawTurns.size());
        for (const auto &[cid, text] : rawTurns) {
            const auto it = nameById.find(cid);
            provenance.script.push_back({it != nameById.end() ? it->second : cid, text});
        }

        // Lip sync: per-creature mouth cues derived from the ElevenLabs forced
        // alignment — the same visemes that drive the mouth servos — embedded so
        // the file carries its own lip sync (#53). Only creature-console reads it.
        auto viseme = getDialogTextToViseme();
        std::unordered_map<std::string, std::pair<uint16_t, std::string>> laneByVoice;
        for (const auto &c : creaturesCache) {
            laneByVoice.emplace(c.voiceId, std::make_pair(c.audioChannel, nameById.at(c.creatureId)));
        }
        for (const auto &pc : assembled.perCreature) {
            const auto it = laneByVoice.find(pc.voiceId);
            if (it == laneByVoice.end()) {
                continue;
            }
            voice::DialogLipsyncTrack lt;
            lt.channel = it->second.first;
            lt.name = it->second.second;
            for (const auto &cue : viseme->charTimingsToMouthCues(pc.mouth)) {
                lt.cues.push_back({cue.start, cue.end, cue.value});
            }
            if (!lt.cues.empty()) {
                provenance.lipsync.push_back(std::move(lt));
            }
        }

        // Word alignment: per-creature word timings on the same tightened timeline
        // as the mouth cues (issue #56, Part 2). The ElevenLabs forced-alignment
        // words survived the assembly shift in DialogPerCreature::words; embed them
        // so the console can look up the word under the mouth-axis cursor. Same lane
        // mapping as the lip sync above.
        for (const auto &pc : assembled.perCreature) {
            const auto it = laneByVoice.find(pc.voiceId);
            if (it == laneByVoice.end() || pc.words.empty()) {
                continue;
            }
            voice::DialogWordTrack wt;
            wt.channel = it->second.first;
            wt.name = it->second.second;
            wt.words = pc.words;
            provenance.wordAlignment.push_back(std::move(wt));
        }
    }

    updateProgress(0.70f);

    // ---- Per-creature base body motion + mouth bytes.
    auto viseme = getDialogTextToViseme();
    SoundDataProcessor soundProc;

    std::optional<uint32_t> msPerFrame;
    std::size_t totalFrames = 0;

    std::vector<voice::CreatureTrackInput> creatureInputs;
    creatureInputs.reserve(assembled.perCreature.size());

    // Every random choice below — which speech loop and idle animation each
    // creature draws, the idle start phases, and each creature's gaze reaction
    // timing — derives from this one seed, which gets stamped into the
    // animation's metadata (#119).
    //
    // That's what makes a stage re-render trustworthy: replay with the same
    // seed against a moved stage and ONLY the gaze bytes change. Without it,
    // nudging one creature would reshuffle everyone's idle animation and you
    // could never see what your edit actually did.
    const uint64_t renderSeed =
        static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::mt19937 rng(static_cast<uint32_t>(renderSeed));

    std::vector<creatures::CreatureRenderChoice> renderChoices;
    renderChoices.reserve(assembled.perCreature.size());

    for (const auto &pc : assembled.perCreature) {
        const DialogJobCreature *cinfo = nullptr;
        for (const auto &c : creaturesCache) {
            if (c.voiceId == pc.voiceId) {
                cinfo = &c;
                break;
            }
        }
        if (!cinfo) {
            return failJob(fmt::format("post-assembly: voice '{}' missing from creature cache", pc.voiceId));
        }

        if (!cinfo->creatureJson.contains("speech_loop_animation_ids") ||
            !cinfo->creatureJson["speech_loop_animation_ids"].is_array() ||
            cinfo->creatureJson["speech_loop_animation_ids"].empty()) {
            return failJob(fmt::format("creature '{}' has no speech_loop_animation_ids", cinfo->creatureId));
        }
        const auto loopIds = cinfo->creatureJson["speech_loop_animation_ids"].get<std::vector<std::string>>();
        std::uniform_int_distribution<std::size_t> dist(0, loopIds.size() - 1);
        const auto chosenId = loopIds[dist(rng)];

        auto baseAnimResult = creatures::db->getAnimation(chosenId, jobState.span);
        if (!baseAnimResult.isSuccess()) {
            return failJob(fmt::format("creature '{}': load base anim {}: {}", cinfo->creatureId, chosenId,
                                       baseAnimResult.getError().value().getMessage()));
        }
        const auto baseAnim = baseAnimResult.getValue().value();

        if (!msPerFrame) {
            msPerFrame = baseAnim.metadata.milliseconds_per_frame;
            if (*msPerFrame == 0) {
                *msPerFrame = 1; // mirror ad-hoc fallback; avoid divide-by-zero
            }
            const double totalMs =
                static_cast<double>(showTimelineSamples) * 1000.0 / static_cast<double>(assembled.sampleRate);
            totalFrames = static_cast<std::size_t>(std::ceil(totalMs / static_cast<double>(*msPerFrame)));
        } else if (baseAnim.metadata.milliseconds_per_frame != *msPerFrame) {
            return failJob(fmt::format(
                "creature '{}': base anim ms/frame {} differs from scene's {}; multi-rate dialog not supported",
                cinfo->creatureId, baseAnim.metadata.milliseconds_per_frame, *msPerFrame));
        }

        auto trackIt = std::find_if(baseAnim.tracks.begin(), baseAnim.tracks.end(),
                                    [&](const Track &t) { return t.creature_id == cinfo->creatureId; });
        if (trackIt == baseAnim.tracks.end()) {
            return failJob(
                fmt::format("creature '{}': base anim {} has no track for this creature", cinfo->creatureId, chosenId));
        }

        std::vector<std::vector<uint8_t>> baseFrames;
        baseFrames.reserve(trackIt->frames.size());
        for (const auto &f : trackIt->frames) {
            baseFrames.push_back(decodeBase64(f));
        }
        if (baseFrames.empty()) {
            return failJob(
                fmt::format("creature '{}': base anim {} track has zero frames", cinfo->creatureId, chosenId));
        }

        // ---- Idle loop for this creature's silent stretches (#119) --------
        // Entirely best-effort: every failure path below just leaves
        // idleFrames empty, and buildSpeechTrack falls back to freezing on
        // baseFrames[0] — the pre-#119 behavior. A creature that can't idle
        // must never fail the render.
        std::vector<std::vector<uint8_t>> idleFrames;
        std::size_t idleStartOffset = 0;
        std::string chosenIdleId;
        if (cinfo->creatureJson.contains("idle_animation_ids") &&
            cinfo->creatureJson["idle_animation_ids"].is_array() &&
            !cinfo->creatureJson["idle_animation_ids"].empty()) {

            const auto idleIds = cinfo->creatureJson["idle_animation_ids"].get<std::vector<std::string>>();
            std::uniform_int_distribution<std::size_t> idleDist(0, idleIds.size() - 1);
            const auto idleChosenId = idleIds[idleDist(rng)];

            auto idleAnimResult = creatures::db->getAnimation(idleChosenId, jobState.span);
            if (!idleAnimResult.isSuccess()) {
                warn("creature '{}': idle anim {} failed to load ({}); freezing during silence instead",
                     cinfo->creatureId, idleChosenId, idleAnimResult.getError().value().getMessage());
            } else {
                const auto idleAnim = idleAnimResult.getValue().value();
                auto idleTrackIt = std::find_if(idleAnim.tracks.begin(), idleAnim.tracks.end(),
                                                [&](const Track &t) { return t.creature_id == cinfo->creatureId; });

                if (idleAnim.metadata.milliseconds_per_frame != *msPerFrame) {
                    // Cycling it anyway would play the idle loop at the wrong
                    // speed. Same guard the speech loop gets, but non-fatal.
                    warn("creature '{}': idle anim {} is {} ms/frame but the scene is {}; freezing during silence "
                         "instead",
                         cinfo->creatureId, idleChosenId, idleAnim.metadata.milliseconds_per_frame, *msPerFrame);
                } else if (idleTrackIt == idleAnim.tracks.end()) {
                    warn("creature '{}': idle anim {} has no track for this creature; freezing during silence instead",
                         cinfo->creatureId, idleChosenId);
                } else {
                    for (const auto &ef : idleTrackIt->frames) {
                        idleFrames.push_back(decodeBase64(ef));
                    }
                    if (idleFrames.empty() || idleFrames.front().size() != baseFrames.front().size()) {
                        warn("creature '{}': idle anim {} frames unusable ({} frames, width {} vs speech width {}); "
                             "freezing during silence instead",
                             cinfo->creatureId, idleChosenId, idleFrames.size(),
                             idleFrames.empty() ? 0 : idleFrames.front().size(), baseFrames.front().size());
                        idleFrames.clear();
                    } else {
                        // Random phase so two creatures that drew the same
                        // idle animation don't loop in lockstep.
                        std::uniform_int_distribution<std::size_t> phaseDist(0, idleFrames.size() - 1);
                        idleStartOffset = phaseDist(rng);
                        chosenIdleId = idleChosenId;
                        debug("creature '{}': idling on anim {} ({} frames) from phase {} during silence",
                              cinfo->creatureId, idleChosenId, idleFrames.size(), idleStartOffset);
                    }
                }
            }
        }

        RhubarbSoundData snd;
        snd.metadata.duration = static_cast<double>(showTimelineSamples) / static_cast<double>(assembled.sampleRate);
        snd.metadata.soundFile = wavPath.filename().string();
        snd.mouthCues = viseme->charTimingsToMouthCues(pc.mouth);
        auto mouthBytes = soundProc.processSoundData(snd, *msPerFrame, totalFrames);

        voice::CreatureTrackInput cti;
        cti.voiceId = pc.voiceId;
        cti.creatureId = cinfo->creatureId;
        cti.creatureJson = cinfo->creatureJson;
        cti.baseFrames = std::move(baseFrames);
        cti.mouthBytes = std::move(mouthBytes);
        cti.idleFrames = std::move(idleFrames);
        cti.idleStartOffset = idleStartOffset;
        creatureInputs.push_back(std::move(cti));

        // Record what this creature actually drew, so a later stage re-render
        // reproduces the same body motion by construction rather than by
        // replaying the rng in exactly the right order (#119).
        creatures::CreatureRenderChoice choice;
        choice.creature_id = cinfo->creatureId;
        choice.speech_loop_animation_id = chosenId;
        choice.idle_animation_id = chosenIdleId;
        choice.idle_start_offset = static_cast<uint32_t>(idleStartOffset);
        renderChoices.push_back(std::move(choice));
    }
    if (!msPerFrame) {
        return failJob("post-assembly: msPerFrame not set (no creatures had usable base animations)");
    }

    // ---- Head aiming (#119) ------------------------------------------------
    // Entirely optional and entirely best-effort: without a stage, or with a
    // stage that doesn't place these creatures, every gaze stream stays empty
    // and the rendered frames are byte-identical to a pre-#119 render.
    // `renderStage` / `haveStage` were resolved up front, alongside the title.
    if (haveStage) {
        const auto placements = creatures::stagePlacements(renderStage);

        // Resolve every creature in the scene that this stage actually places.
        std::vector<voice::GazeGeometry> geometries;
        std::unordered_map<std::string, std::size_t> geometryByCreature;
        for (const auto &placement : placements) {
            auto creatureResult = creatures::db->getCreature(placement.creature_id, jobState.span);
            if (!creatureResult.isSuccess() || !creatureResult.getValue().has_value()) {
                warn("Dialog job {}: stage {} places unknown creature {}; skipping it", jobState.jobId, stageId,
                     placement.creature_id);
                continue;
            }
            auto geometry = voice::resolveGazeGeometry(creatureResult.getValue().value(), placement);
            geometryByCreature.emplace(placement.creature_id, geometries.size());
            geometries.push_back(std::move(geometry));
        }

        // Who holds the floor when, derived from the same mouth-byte streams
        // the tracks are built from. Merge gaps up to the body tail so
        // between-word silence doesn't shred a turn into dozens of spans.
        std::vector<std::string> timelineIds;
        std::vector<std::span<const uint8_t>> timelineMouths;
        timelineIds.reserve(creatureInputs.size());
        timelineMouths.reserve(creatureInputs.size());
        for (const auto &cti : creatureInputs) {
            timelineIds.push_back(cti.creatureId);
            timelineMouths.emplace_back(cti.mouthBytes);
        }
        const std::size_t gapTolerance = std::max<std::size_t>(1, 400 / std::max<uint32_t>(1, *msPerFrame));
        const auto timeline = voice::buildSpeakerTimeline(timelineIds, timelineMouths, totalFrames, gapTolerance);

        std::size_t aimedCreatures = 0;
        for (auto &cti : creatureInputs) {
            auto it = geometryByCreature.find(cti.creatureId);
            if (it == geometryByCreature.end()) {
                continue; // not placed on this stage; it just won't look around
            }
            // Per-creature rng stream. Seeded off the shared generator so the
            // whole render stays reproducible from one seed, but drawn per
            // creature so each gets independent reaction timing — which is the
            // entire point of the jitter model.
            std::mt19937 gazeRng(static_cast<uint32_t>(rng()));
            auto gaze =
                voice::buildGazeTrack(geometries[it->second], geometries, timeline, totalFrames, *msPerFrame, gazeRng);
            if (gaze.empty()) {
                continue; // creature has no gaze config
            }
            cti.gazePanBytes = std::move(gaze.panBytes);
            cti.gazeElevationBytes = std::move(gaze.elevationBytes);
            cti.gazeCockBytes = std::move(gaze.cockBytes);
            cti.gazePanSlot = gaze.panSlot;
            cti.gazeElevationSlot = gaze.elevationSlot;
            cti.gazeCockSlot = gaze.cockSlot;
            ++aimedCreatures;
        }

        info("Dialog job {}: stage '{}' aimed {} of {} creatures over {} speaker spans", jobState.jobId,
             renderStage.title, aimedCreatures, creatureInputs.size(), timeline.size());
        if (jobState.span) {
            jobState.span->setAttribute("dialog.stage_id", stageId);
            jobState.span->setAttribute("dialog.stage_title", renderStage.title);
            jobState.span->setAttribute("dialog.aimed_creatures", static_cast<int64_t>(aimedCreatures));
        }
    }

    updateProgress(0.85f);

    // ---- Re-render dedupe: if this scene is being rendered FROM a script and
    // we're writing to the permanent collection, look up any prior animation
    // produced from this same script. If one exists, reuse its id so the
    // upsert below overwrites the existing document in place rather than
    // accumulating duplicates. AdHoc renders skip this — they have TTL and
    // each render is conceptually a fresh take.
    std::string existingAnimationId;
    std::string supersededSoundFile;
    if (!sourceScriptId.empty() && persistence == DialogPersistence::Permanent) {
        auto lookup = creatures::db->findAnimationIdBySourceScriptId(sourceScriptId, stageId, jobState.span);
        if (lookup.isSuccess() && lookup.getValue().value().has_value()) {
            existingAnimationId = lookup.getValue().value().value();
            // Remember what this animation currently points at. The render
            // below writes a NEW audio file and repoints the animation, so
            // without this the old one is orphaned on disk forever — and
            // these run to hundreds of MB (#128).
            if (auto previous = creatures::db->getAnimation(existingAnimationId, jobState.span);
                previous.isSuccess() && previous.getValue().has_value()) {
                supersededSoundFile = previous.getValue().value().metadata.sound_file;
            }
            info("Dialog job {}: re-rendering existing animation {} for script {}", jobState.jobId, existingAnimationId,
                 sourceScriptId);
            if (jobState.span) {
                jobState.span->setAttribute("animation.reused_id", existingAnimationId);
            }
        }
        // Lookup failures are non-fatal — fall through to fresh-id behavior.
        // The worst case is one orphaned animation, which is what we had before
        // this dedupe existed.
    }

    // ---- Build the multi-track Animation.
    auto animResult = voice::buildDialogAnimation(assembled, creatureInputs, *msPerFrame, animationSoundFile, title,
                                                  jobState.span, existingAnimationId, showTimelineSamples);
    if (!animResult.isSuccess()) {
        return failJob(animResult.getError().value().getMessage());
    }
    auto animation = animResult.getValue().value();

    // Stamp the script provenance onto the Animation metadata. These are two
    // independent things (#60): the CoW snapshot (source_script_turns) preserves
    // what was rendered — it's stamped for every dialog render, inline or saved,
    // so an edit-then-delete doesn't orphan the animation's history. The soft
    // pointer (source_script_id) only exists for saved-script renders and lets the
    // UI offer "edit this script" + drives re-render dedupe.
    if (!sourceScriptTurns.empty()) {
        animation.metadata.source_script_turns = sourceScriptTurns;
    }
    if (!sourceScriptId.empty()) {
        animation.metadata.source_script_id = sourceScriptId;
        if (jobState.span) {
            jobState.span->setAttribute("animation.source_script_id", sourceScriptId);
        }
    }

    // Stage provenance (#119). The seed is stamped unconditionally — it
    // describes how THIS render was produced whether or not a stage was
    // involved. The stage pointer and its updated_at only when one was bound;
    // together they answer "is this animation stale?" with a comparison
    // instead of a diff.
    animation.metadata.render_seed = renderSeed;
    animation.metadata.source_render_choices = renderChoices;
    if (haveStage) {
        animation.metadata.source_stage_id = renderStage.id;
        animation.metadata.source_stage_updated_at = renderStage.updated_at;
        if (jobState.span) {
            jobState.span->setAttribute("animation.source_stage_id", renderStage.id);
        }
    }

    // Validate and construct every animation frame before publishing the much
    // larger multichannel WAV. From this point until the database publication
    // succeeds, any return or exception removes the new/partial file.
    const bool embedProvenance = persistence == DialogPersistence::Permanent && !provenance.empty();
    RemoveFileUnlessReleased wavCleanup(wavPath);
    auto wavWriteResult = voice::writeDialogWav(assembled, voiceToChannel, wavPath, jobState.span,
                                                embedProvenance ? &provenance : nullptr, backgroundMusicSamples);
    if (!wavWriteResult.isSuccess()) {
        return failJob(wavWriteResult.getError().value().getMessage());
    }
    updateProgress(0.90f);

    // ---- Persist via the storage facade so the right cache invalidations
    // fire automatically per persistence (AdHoc → AdHocAnimationList+AdHocSoundList,
    // Permanent → Animation+SoundList). Per issue #11 we can't forget any of them.
    if (persistence == DialogPersistence::AdHoc) {
        auto insertResult = creatures::storage::publishAdHocAnimation(animation, jobState.span);
        if (!insertResult.isSuccess()) {
            return failJob(fmt::format("publishAdHocAnimation: {}", insertResult.getError().value().getMessage()));
        }
    } else {
        const auto j = animationToJson(animation);
        auto upsertResult = creatures::storage::publishAnimation(j.dump(), jobState.span);
        if (!upsertResult.isSuccess()) {
            return failJob(fmt::format("publishAnimation: {}", upsertResult.getError().value().getMessage()));
        }
    }
    wavCleanup.release();

    // The new audio is committed and the animation points at it, so the file
    // it used to point at is now unreferenced (#128). Delete it — a long scene
    // is hundreds of MB and every re-render used to leave one behind.
    //
    // Deliberately after the publish succeeded: losing the old file while the
    // new one failed to land would leave the animation pointing at nothing.
    if (!supersededSoundFile.empty() && supersededSoundFile != animation.metadata.sound_file) {
        auto refs = creatures::db->countAnimationsBySoundFile(supersededSoundFile, jobState.span);
        if (!refs.isSuccess()) {
            warn("Dialog job {}: could not check references for superseded sound '{}' ({}); leaving it in place",
                 jobState.jobId, supersededSoundFile, refs.getError().value().getMessage());
        } else if (refs.getValue().value() > 0) {
            // Something still points at it — a hand-edited animation, most
            // likely. Never delete audio another animation is using.
            info("Dialog job {}: superseded sound '{}' is still referenced by {} animation(s); leaving it",
                 jobState.jobId, supersededSoundFile, refs.getValue().value());
        } else {
            auto removed = creatures::storage::deleteSupersededDialogSound(supersededSoundFile, jobState.span);
            if (!removed.isSuccess()) {
                warn("Dialog job {}: cleanup of '{}' failed: {}", jobState.jobId, supersededSoundFile,
                     removed.getError().value().getMessage());
            }
        }
    }

    updateProgress(0.95f);

    // ---- Optional autoplay. Universe was validated above to be common across
    // all creatures, so pull it from the first.
    bool autoplayed = false;
    if (autoplay && !creaturesCache.empty()) {
        const auto universe = creaturesCache.front().universe;
        // interrupt() wants a RequestSpan parent; the worker only has a job
        // OperationSpan, so pass nullptr (ad-hoc path does the same).
        auto interruptResult = creatures::sessionManager->interrupt(universe, animation, false, nullptr);
        if (!interruptResult.isSuccess()) {
            warn("Dialog job {}: persisted as {} but autoplay interrupt() failed: {}", jobState.jobId, animation.id,
                 interruptResult.getError().value().getMessage());
            // Don't fail the job — the animation is safely stored. `autoplayed`
            // stays false so the client can tell why playback didn't fire.
        } else {
            info("Dialog job {}: autoplay interrupted universe {} with animation {}", jobState.jobId, universe,
                 animation.id);
            autoplayed = true;
        }
    }

    // ---- Success. Build the typed result DTO and let oatpp serialize it for
    // the framework's string-shaped JobState::result field.
    auto resultDto = ws::DialogJobResultDto::createShared();
    resultDto->animation_id = animation.id.c_str();
    resultDto->number_of_frames = animation.metadata.number_of_frames;
    resultDto->milliseconds_per_frame = animation.metadata.milliseconds_per_frame;
    resultDto->duration_seconds = static_cast<double>(animation.metadata.number_of_frames) *
                                  static_cast<double>(animation.metadata.milliseconds_per_frame) / 1000.0;
    resultDto->persistence = (persistence == DialogPersistence::AdHoc) ? "adhoc" : "permanent";
    resultDto->autoplayed = autoplayed;
    jobManager_->completeJob(jobState.jobId, jsonMapper->writeToString(resultDto)->c_str());
    if (jobState.span) {
        jobState.span->setAttribute("dialog.animation_id", animation.id);
        jobState.span->setSuccess();
    }
    info("Dialog job {} succeeded: animation_id={}", jobState.jobId, animation.id);
    broadcastCompletion(jobState.jobId);
}

// ===========================================================================
// Dialog preview handlers — async wrappers around DialogPreviewService.
// ===========================================================================

void JobWorker::handleDialogPreviewJob(JobState &jobState) {
    auto broadcastProgress = [this](const std::string &jobId) {
        auto updated = jobManager_->getJob(jobId);
        if (updated) {
            auto r = broadcastJobProgressToAllClients(*updated);
            if (!r.isSuccess()) {
                warn("Failed to broadcast dialog preview job progress: {}", r.getError()->getMessage());
            }
        }
    };
    auto broadcastCompletion = [this](const std::string &jobId) {
        auto updated = jobManager_->getJob(jobId);
        if (updated) {
            auto r = broadcastJobCompleteToAllClients(*updated);
            if (!r.isSuccess()) {
                warn("Failed to broadcast dialog preview job completion: {}", r.getError()->getMessage());
            }
        }
    };
    auto updateProgress = [&](float v) {
        jobManager_->updateJobProgress(jobState.jobId, v);
        broadcastProgress(jobState.jobId);
    };
    auto failJob = [&](const std::string &msg) {
        error("Dialog preview job {} failed: {}", jobState.jobId, msg);
        if (jobState.span) {
            jobState.span->setError(msg);
        }
        jobManager_->failJob(jobState.jobId, msg);
        broadcastCompletion(jobState.jobId);
    };

    auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
    oatpp::Object<ws::DialogPreviewRequestDto> reqDto;
    try {
        reqDto = jsonMapper->readFromString<oatpp::Object<ws::DialogPreviewRequestDto>>(jobState.details.c_str());
    } catch (const std::exception &e) {
        return failJob(fmt::format("invalid dialog preview job details: {}", e.what()));
    }
    if (!reqDto) {
        return failJob("dialog preview job details deserialized to null");
    }

    updateProgress(0.05f);

    ws::DialogPreviewService service;
    auto progress = [&](float f) { updateProgress(0.05f + 0.90f * std::clamp(f, 0.0f, 1.0f)); };
    auto outcomeResult = service.loadOrGenerate(reqDto, jobState.span, "meta-job", progress, jobState.jobId);
    if (!outcomeResult.isSuccess()) {
        return failJob(outcomeResult.getError().value().getMessage());
    }
    const auto outcome = outcomeResult.getValue().value();

    // Every generated take is an ad-hoc sound (#131): browsable and
    // re-auditionable for 24 h, and the file acceptance later promotes into
    // the permanent tree. Writing it here rather than only in the explicit
    // export job is what makes "generate four takes, come back tomorrow, pick
    // one" work. Non-fatal: a scene whose creatures have no usable
    // audio_channel still has perfectly good metadata to return, and accept
    // will say so precisely if it's ever asked to promote this take.
    updateProgress(0.95f);
    auto exportResult = service.ensureAdHocExport(outcome, jobState.span);
    if (!exportResult.isSuccess()) {
        warn("dialog preview job {}: could not write the ad-hoc take export: {}", jobState.jobId,
             exportResult.getError().value().getMessage());
    }

    auto dto = ws::DialogPreviewMetaResponseDto::createShared();
    ws::DialogPreviewService::populateMetaResponse(dto, outcome.generation, outcome.cacheKey, outcome.cached);

    updateProgress(1.0f);
    jobManager_->completeJob(jobState.jobId, jsonMapper->writeToString(dto)->c_str());
    if (jobState.span) {
        jobState.span->setAttribute("dialog.generation_id", outcome.generation.generationId);
        jobState.span->setSuccess();
    }
    info("Dialog preview job {} succeeded: generation_id={}", jobState.jobId, outcome.generation.generationId);
    broadcastCompletion(jobState.jobId);
}

void JobWorker::handleDialogPreviewExportJob(JobState &jobState) {
    auto broadcastProgress = [this](const std::string &jobId) {
        auto updated = jobManager_->getJob(jobId);
        if (updated) {
            auto r = broadcastJobProgressToAllClients(*updated);
            if (!r.isSuccess()) {
                warn("Failed to broadcast dialog preview export job progress: {}", r.getError()->getMessage());
            }
        }
    };
    auto broadcastCompletion = [this](const std::string &jobId) {
        auto updated = jobManager_->getJob(jobId);
        if (updated) {
            auto r = broadcastJobCompleteToAllClients(*updated);
            if (!r.isSuccess()) {
                warn("Failed to broadcast dialog preview export job completion: {}", r.getError()->getMessage());
            }
        }
    };
    auto updateProgress = [&](float v) {
        jobManager_->updateJobProgress(jobState.jobId, v);
        broadcastProgress(jobState.jobId);
    };
    auto failJob = [&](const std::string &msg) {
        error("Dialog preview export job {} failed: {}", jobState.jobId, msg);
        if (jobState.span) {
            jobState.span->setError(msg);
        }
        jobManager_->failJob(jobState.jobId, msg);
        broadcastCompletion(jobState.jobId);
    };

    auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
    oatpp::Object<ws::DialogPreviewRequestDto> reqDto;
    try {
        reqDto = jsonMapper->readFromString<oatpp::Object<ws::DialogPreviewRequestDto>>(jobState.details.c_str());
    } catch (const std::exception &e) {
        return failJob(fmt::format("invalid dialog preview export job details: {}", e.what()));
    }
    if (!reqDto) {
        return failJob("dialog preview export job details deserialized to null");
    }

    updateProgress(0.05f);

    ws::DialogPreviewService service;
    // loadOrGenerate owns 0.05..0.70; the WAV assembly owns the rest.
    auto progress = [&](float f) { updateProgress(0.05f + 0.65f * std::clamp(f, 0.0f, 1.0f)); };
    auto outcomeResult = service.loadOrGenerate(reqDto, jobState.span, "multichannel-job", progress, jobState.jobId);
    if (!outcomeResult.isSuccess()) {
        return failJob(outcomeResult.getError().value().getMessage());
    }
    const auto outcome = outcomeResult.getValue().value();
    updateProgress(0.70f);

    // Write the 17-channel WAV into the ad-hoc sound bucket so it's downloadable
    // through GET /api/v1/sound/ad-hoc/{filename} and shareable for free.
    // Generation writes this file too (#131), so this is usually a no-op that
    // just hands back the path.
    auto exportResult = service.ensureAdHocExport(outcome, jobState.span);
    if (!exportResult.isSuccess()) {
        return failJob(exportResult.getError().value().getMessage());
    }
    const auto fileName = exportResult.getValue().value().filename().string();
    updateProgress(0.95f);

    auto resultDto = ws::DialogPreviewExportResultDto::createShared();
    resultDto->file_name = fileName.c_str();
    resultDto->generation_id = outcome.generation.generationId.c_str();
    resultDto->cache_key = outcome.cacheKey.c_str();

    updateProgress(1.0f);
    jobManager_->completeJob(jobState.jobId, jsonMapper->writeToString(resultDto)->c_str());
    if (jobState.span) {
        jobState.span->setAttribute("dialog.generation_id", outcome.generation.generationId);
        jobState.span->setAttribute("export.file_name", fileName);
        jobState.span->setSuccess();
    }
    info("Dialog preview export job {} succeeded: file_name={}", jobState.jobId, fileName);
    broadcastCompletion(jobState.jobId);
}

void JobWorker::handleVoiceFileJob(JobState &jobState) {
    auto broadcastCompletion = [this](const std::string &jobId) {
        auto updated = jobManager_->getJob(jobId);
        if (updated) {
            auto r = broadcastJobCompleteToAllClients(*updated);
            if (!r.isSuccess()) {
                warn("Failed to broadcast voice file job completion: {}", r.getError()->getMessage());
            }
        }
    };
    auto broadcastProgress = [this](const std::string &jobId) {
        auto updated = jobManager_->getJob(jobId);
        if (updated) {
            auto r = broadcastJobProgressToAllClients(*updated);
            if (!r.isSuccess()) {
                warn("Failed to broadcast voice file job progress: {}", r.getError()->getMessage());
            }
        }
    };
    auto updateProgress = [&](float v) {
        jobManager_->updateJobProgress(jobState.jobId, v);
        broadcastProgress(jobState.jobId);
    };
    auto failJob = [&](const std::string &msg) {
        error("Voice file job {} failed: {}", jobState.jobId, msg);
        if (jobState.span) {
            jobState.span->setError(msg);
        }
        jobManager_->failJob(jobState.jobId, msg);
        broadcastCompletion(jobState.jobId);
    };

    auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
    oatpp::Object<ws::MakeSoundFileRequestDto> reqDto;
    try {
        reqDto = jsonMapper->readFromString<oatpp::Object<ws::MakeSoundFileRequestDto>>(jobState.details.c_str());
    } catch (const std::exception &e) {
        return failJob(fmt::format("invalid voice file job details: {}", e.what()));
    }
    if (!reqDto || !reqDto->creature_id || reqDto->creature_id->empty() || !reqDto->text || reqDto->text->empty()) {
        return failJob("voice file jobs require both creature_id and text");
    }

    if (jobState.span) {
        jobState.span->setAttribute("creature.id", std::string(*reqDto->creature_id));
        const auto text = std::string(*reqDto->text);
        jobState.span->setAttribute("speech.text_length", static_cast<int64_t>(text.size()));
        jobState.span->setAttribute("speech.text_preview", text.substr(0, 60));
    }

    updateProgress(0.05f);

    ws::VoiceService voiceService;
    oatpp::Object<creatures::voice::CreatureSpeechResponseDto> response;
    try {
        response = voiceService.generateCreatureSpeech(reqDto);
    } catch (const std::exception &e) {
        return failJob(fmt::format("speech generation failed: {}", e.what()));
    }
    if (!response) {
        return failJob("speech generation returned no response");
    }

    // The underlying CreatureVoicesLib write doesn't go through the storage
    // facade's writeSoundFile yet, so fire the SoundList invalidation manually
    // (mirrors what the old synchronous controller did).
    creatures::storage::broadcastCacheInvalidation(CacheType::SoundList);

    updateProgress(1.0f);
    jobManager_->completeJob(jobState.jobId, jsonMapper->writeToString(response)->c_str());
    if (jobState.span) {
        jobState.span->setSuccess();
    }
    info("Voice file job {} succeeded", jobState.jobId);
    broadcastCompletion(jobState.jobId);
}

void JobWorker::handleVoiceTakeAcceptJob(JobState &jobState) {
    auto broadcastProgress = [this](const std::string &jobId) {
        auto updated = jobManager_->getJob(jobId);
        if (updated) {
            auto r = broadcastJobProgressToAllClients(*updated);
            if (!r.isSuccess()) {
                warn("Failed to broadcast voice take accept progress: {}", r.getError()->getMessage());
            }
        }
    };
    auto broadcastCompletion = [this](const std::string &jobId) {
        auto updated = jobManager_->getJob(jobId);
        if (updated) {
            auto r = broadcastJobCompleteToAllClients(*updated);
            if (!r.isSuccess()) {
                warn("Failed to broadcast voice take accept completion: {}", r.getError()->getMessage());
            }
        }
    };
    auto updateProgress = [&](float v) {
        jobManager_->updateJobProgress(jobState.jobId, v);
        broadcastProgress(jobState.jobId);
    };
    auto failJob = [&](const std::string &msg) {
        error("Voice take accept job {} failed: {}", jobState.jobId, msg);
        if (jobState.span) {
            jobState.span->setError(msg);
        }
        jobManager_->failJob(jobState.jobId, msg);
        broadcastCompletion(jobState.jobId);
    };

    std::string scriptId, generationId, cacheKey;
    try {
        auto details = nlohmann::json::parse(jobState.details);
        scriptId = details.value("script_id", std::string{});
        generationId = details.value("generation_id", std::string{});
        cacheKey = details.value("dialog_cache_key", std::string{});
    } catch (const std::exception &e) {
        return failJob(fmt::format("could not parse job details: {}", e.what()));
    }
    if (scriptId.empty() || generationId.empty() || cacheKey.empty()) {
        return failJob("voice take accept job needs script_id, generation_id and dialog_cache_key");
    }
    if (jobState.span) {
        jobState.span->setAttribute("script.id", scriptId);
        jobState.span->setAttribute("dialog.generation_id", generationId);
        jobState.span->setAttribute("dialog.cache_key", cacheKey);
    }

    updateProgress(0.05f);

    auto existing = creatures::db->getDialogScript(scriptId, jobState.span);
    if (!existing.isSuccess()) {
        return failJob(existing.getError().value().getMessage());
    }
    auto script = existing.getValue().value();

    // The controller checked this before enqueuing, but assembly takes long
    // enough that the turns can change underneath us. Re-check rather than
    // stamp an acceptance that is stale the moment it lands.
    auto currentKey = creatures::voice::computeScriptCacheKey(script.turns, jobState.span);
    if (!currentKey.isSuccess()) {
        return failJob(currentKey.getError().value().getMessage());
    }
    if (currentKey.getValue().value() != cacheKey) {
        return failJob("the script's turns changed while this take's audio was being assembled — re-audition and "
                       "accept again");
    }

    updateProgress(0.10f);

    // The expensive part, and the only reason this is a job: a long scene's
    // 17-channel WAV runs to hundreds of MB. Reads the cached generation, so
    // no ElevenLabs call and no change of performance.
    ws::DialogPreviewService previewService;
    auto exported = previewService.ensureAdHocExportForTake(script.turns, cacheKey, generationId, jobState.span);
    if (!exported.isSuccess()) {
        return failJob(exported.getError().value().getMessage());
    }

    updateProgress(0.85f);

    // Make the take durable BEFORE anything is demoted or published (#146).
    //
    // Acceptance has to mean "this exact performance, forever", but the
    // generation only exists in temp space that a cron sweep or a reboot may
    // delete. Once that happens the render finds nothing and quietly
    // regenerates a DIFFERENT take. Copy it somewhere permanent first, and
    // fail the acceptance outright if we can't — an acceptance we cannot
    // honour later is worse than no acceptance at all.
    auto madeDurable = creatures::voice::saveAcceptedGeneration(cacheKey, generationId);
    if (!madeDurable.isSuccess()) {
        return failJob(
            fmt::format("could not store the accepted take durably: {}", madeDurable.getError().value().getMessage()));
    }

    // Demote after the assembly, so a failure above leaves the previously
    // accepted take whole rather than half-moved.
    if (script.accepted_voice && script.accepted_voice->generation_id != generationId) {
        auto demoted = creatures::storage::demoteVoiceTake(script.accepted_voice->sound_file,
                                                           script.accepted_voice->generation_id, jobState.span);
        if (!demoted.isSuccess()) {
            return failJob(demoted.getError().value().getMessage());
        }
        // Best effort — a leftover durable copy costs disk, not correctness.
        creatures::voice::removeAcceptedGeneration(script.accepted_voice->dialog_cache_key,
                                                   script.accepted_voice->generation_id);
    }

    const auto filename = creatures::util::exportBasename(script.title, generationId) + ".wav";
    auto promoted = creatures::storage::promoteVoiceTake(generationId, filename, jobState.span);
    if (!promoted.isSuccess()) {
        return failJob(promoted.getError().value().getMessage());
    }

    creatures::AcceptedVoice accepted;
    accepted.generation_id = generationId;
    accepted.dialog_cache_key = cacheKey;
    accepted.sound_file = promoted.getValue().value().forMetadata;
    accepted.accepted_at =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    script.accepted_voice = accepted;

    auto updated = creatures::dialogScriptToJson(script);
    updated["updated_at"] = std::max(accepted.accepted_at, script.updated_at + 1);
    auto published = creatures::storage::publishDialogScript(updated.dump(), jobState.span);
    if (!published.isSuccess()) {
        return failJob(published.getError().value().getMessage());
    }

    // Same body the synchronous accept returns, so a client that got a 202
    // ends up with exactly what a 200 would have given it.
    auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
    auto dto = creatures::convertToDto(published.getValue().value());

    updateProgress(1.0f);
    jobManager_->completeJob(jobState.jobId, jsonMapper->writeToString(dto)->c_str());
    if (jobState.span) {
        jobState.span->setAttribute("voice.sound_file", accepted.sound_file);
        jobState.span->setSuccess();
    }
    info("accepted voice take {} for script '{}' -> {} (job {})", generationId, script.title, accepted.sound_file,
         jobState.jobId);
    broadcastCompletion(jobState.jobId);
}

} // namespace creatures::jobs
