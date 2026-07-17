
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
#include "server/animation/SessionManager.h"
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
#include "server/voice/LipSyncProcessor.h"
#include "server/voice/RhubarbData.h"
#include "server/voice/SoundDataProcessor.h"
#include "server/voice/SpeechGenerationManager.h"
#include "server/voice/SpeechTrackBuilder.h"
#include "server/voice/StreamingSpeechGenerationManager.h"
#include "server/voice/TextToViseme.h"
#include "server/voice/WavFileReader.h"
#include "server/ws/dto/DialogDto.h"
#include "server/ws/dto/DialogPreviewExportResultDto.h"
#include "server/ws/dto/MakeSoundFileRequestDto.h"
#include "server/ws/service/DialogPreviewService.h"
#include "server/ws/service/VoiceService.h"

#include <model/CreatureSpeechResponse.h>

#include "util/ObservabilityManager.h"
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

// getAdHocTempRoot + getAnimationLipSyncTempRoot used to live here as anon-
// namespace helpers (the latter for the lipsync handler, the former for ad-hoc
// speech + the dialog handler). All callers migrated to the storage facade in
// issue #11 — see creatures::storage::{root, allocateSoundPath}.

std::string slugify(const std::string &value, std::size_t maxLength = 40) {
    std::string slug;
    slug.reserve(std::min<std::size_t>(value.size(), maxLength));
    bool lastDash = false;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            slug.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            lastDash = false;
        } else if (std::isspace(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            if (!lastDash && !slug.empty()) {
                slug.push_back('-');
                lastDash = true;
            }
        }
        if (slug.size() >= maxLength) {
            break;
        }
    }
    if (slug.empty()) {
        slug = "speech";
    }
    if (slug.back() == '-') {
        slug.pop_back();
    }
    return slug;
}

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

    auto buffer = creatures::rtp::AudioStreamBuffer::loadFromWavFile(wavPath.string(), span);
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
        case JobType::VoiceFile:
            info("Handling job {} as VoiceFile type", jobId);
            handleVoiceFileJob(jobState);
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

        const std::string trackSlug = slugify(creatureId.empty() ? fmt::format("track{}", idx) : creatureId);
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

        auto trackResult = processor.replaceAxisDataWithSoundData(rhubarbData, creature.mouth_slot, track,
                                                                  animation.metadata.milliseconds_per_frame);
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
        auto textSlug = slugify(text);
        auto timestamp = fmt::format("{:%Y%m%d%H%M%S}", std::chrono::system_clock::now());

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
            auto creatureSlug = slugify(creatureName);
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
            auto creatureSlug = slugify(creatureName);
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

        if (config->getAnimationSchedulerType() != Configuration::AnimationSchedulerType::Cooperative) {
            failJob("Ad-hoc speech requires the cooperative scheduler");
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
        trackInput.mouthSlot = creature.mouth_slot;
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
/// (see LocalSdlAudioTransport.cpp).
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
    std::vector<creatures::DialogScriptTurn> sourceScriptTurns;
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
    if (title.empty()) {
        title = fmt::format("Dialog {}", jobState.jobId);
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
    auto chunksResult = voice::chunkTurns(inputs);
    if (!chunksResult.isSuccess()) {
        return failJob(chunksResult.getError().value().getMessage());
    }
    const auto chunks = chunksResult.getValue().value();
    if (jobState.span) {
        jobState.span->setAttribute("dialog.chunks", static_cast<int64_t>(chunks.size()));
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

        // Only honor the explicit generation_id on a SINGLE-chunk scene —
        // for multi-chunk scenes the id would only match one chunk's cache
        // anyway, and we don't want to confuse the user about which chunk
        // got reused.
        const bool useExplicitId = !requestedGenerationId.empty() && chunks.size() == 1;
        if (useExplicitId) {
            auto loadResult = voice::loadGeneration(cacheKey, requestedGenerationId);
            if (loadResult.isSuccess()) {
                auto gen = loadResult.getValue().value();
                chunkGenerationId = gen.generationId;
                chunkAudio = std::move(gen.audioPcm);
                chunkSegments = std::move(gen.voiceSegments);
                chunkAlignment = std::move(gen.forcedAlignment);
                cacheHit = true;
                info("Dialog job {}: chunk {} using requested generation_id={}", jobState.jobId, ci,
                     requestedGenerationId);
            } else {
                warn("Dialog job {}: requested generation_id={} not in cache for this chunk — regenerating",
                     jobState.jobId, requestedGenerationId);
            }
        }
        if (!cacheHit) {
            if (auto latest = voice::findLatestGeneration(cacheKey)) {
                auto loadResult = voice::loadGeneration(cacheKey, *latest);
                if (loadResult.isSuccess()) {
                    auto gen = loadResult.getValue().value();
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
        }
        if (chunkSpan) {
            chunkSpan->setAttribute("dialog.cache_hit", cacheHit);
        }
        generationIds.push_back(chunkGenerationId);

        // Reassemble the DialogResult shape that assembleChunk expects.
        voice::DialogResult dialog;
        dialog.audioData = std::move(chunkAudio);
        dialog.audioFormat = "pcm_48000";
        dialog.voiceSegments = std::move(chunkSegments);

        auto assembleResult = voice::assembleChunk(chunk, dialog, chunkAlignment, kDialogSampleRate);
        if (!assembleResult.isSuccess()) {
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

    // ---- 17-channel WAV output. The storage facade owns the path math AND
    // the absolute-vs-relative metadata convention (Permanent stores relative,
    // AdHoc stores absolute) so this handler doesn't reinvent it.
    const auto wavBucket = persistence == DialogPersistence::AdHoc ? creatures::storage::Persistence::AdHoc
                                                                   : creatures::storage::Persistence::Permanent;
    const auto wavFilename = persistence == DialogPersistence::AdHoc ? fmt::format("dialog_{}.wav", jobState.jobId)
                                                                     : fmt::format("{}.wav", jobState.jobId);
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
    voice::DialogWavProvenance provenance;
    if (persistence == DialogPersistence::Permanent) {
        provenance.sourceScriptId = sourceScriptId;
        provenance.title = title;
        provenance.generationIds = generationIds;

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

    const bool embedProvenance = persistence == DialogPersistence::Permanent && !provenance.empty();
    auto wavWriteResult = voice::writeDialogWav(assembled, voiceToChannel, wavPath, jobState.span,
                                                embedProvenance ? &provenance : nullptr);
    if (!wavWriteResult.isSuccess()) {
        return failJob(wavWriteResult.getError().value().getMessage());
    }
    updateProgress(0.70f);

    // ---- Per-creature base body motion + mouth bytes.
    auto viseme = getDialogTextToViseme();
    SoundDataProcessor soundProc;

    std::optional<uint32_t> msPerFrame;
    std::size_t totalFrames = 0;

    std::vector<voice::CreatureTrackInput> creatureInputs;
    creatureInputs.reserve(assembled.perCreature.size());

    std::mt19937 rng(static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));

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
                static_cast<double>(assembled.totalSamples) * 1000.0 / static_cast<double>(assembled.sampleRate);
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

        RhubarbSoundData snd;
        snd.metadata.duration = static_cast<double>(assembled.totalSamples) / static_cast<double>(assembled.sampleRate);
        snd.metadata.soundFile = wavPath.filename().string();
        snd.mouthCues = viseme->charTimingsToMouthCues(pc.mouth);
        auto mouthBytes = soundProc.processSoundData(snd, *msPerFrame, totalFrames);

        voice::CreatureTrackInput cti;
        cti.voiceId = pc.voiceId;
        cti.creatureId = cinfo->creatureId;
        cti.creatureJson = cinfo->creatureJson;
        cti.baseFrames = std::move(baseFrames);
        cti.mouthBytes = std::move(mouthBytes);
        creatureInputs.push_back(std::move(cti));
    }
    if (!msPerFrame) {
        return failJob("post-assembly: msPerFrame not set (no creatures had usable base animations)");
    }
    updateProgress(0.85f);

    // ---- Re-render dedupe: if this scene is being rendered FROM a script and
    // we're writing to the permanent collection, look up any prior animation
    // produced from this same script. If one exists, reuse its id so the
    // upsert below overwrites the existing document in place rather than
    // accumulating duplicates. AdHoc renders skip this — they have TTL and
    // each render is conceptually a fresh take.
    std::string existingAnimationId;
    if (!sourceScriptId.empty() && persistence == DialogPersistence::Permanent) {
        auto lookup = creatures::db->findAnimationIdBySourceScriptId(sourceScriptId, jobState.span);
        if (lookup.isSuccess() && lookup.getValue().value().has_value()) {
            existingAnimationId = lookup.getValue().value().value();
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
                                                  jobState.span, existingAnimationId);
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
    auto adHocRootResult = creatures::storage::root(creatures::storage::Persistence::AdHoc);
    if (!adHocRootResult.isSuccess()) {
        return failJob(fmt::format("Unable to access ad-hoc root: {}", adHocRootResult.getError()->getMessage()));
    }
    const auto exportDir = adHocRootResult.getValue().value() / "preview-exports";
    const auto fileName = fmt::format("dialog-17ch-{}.wav", outcome.generation.generationId);
    const auto wavPath = exportDir / fileName;

    auto exportResult = service.exportMultichannel(outcome, wavPath, jobState.span);
    if (!exportResult.isSuccess()) {
        return failJob(exportResult.getError().value().getMessage());
    }
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

} // namespace creatures::jobs
