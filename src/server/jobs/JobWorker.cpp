
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
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/voice/AudioConverter.h"
#include "server/voice/LipSyncProcessor.h"
#include "server/voice/RhubarbData.h"
#include "server/voice/SoundDataProcessor.h"
#include "server/voice/SpeechGenerationManager.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"
#include "util/helpers.h"
#include "util/threadName.h"
#include "util/uuidUtils.h"
#include "util/websocketUtils.h"

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

std::filesystem::path getAdHocTempRoot() { return std::filesystem::temp_directory_path() / "creature-adhoc"; }
std::filesystem::path getAnimationLipSyncTempRoot() { return std::filesystem::temp_directory_path() / "creature-lipsync"; }

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

    auto channelCountResult =
        voice::AudioConverter::getChannelCount(audioPath, config->getFfmpegBinaryPath(), jobState.span);
    if (!channelCountResult.isSuccess()) {
        failJob(channelCountResult.getError()->getMessage());
        return;
    }
    const auto channelCount = channelCountResult.getValue().value();
    if (channelCount != RTP_STREAMING_CHANNELS) {
        failJob(fmt::format("Expected {} channels but audio has {}", RTP_STREAMING_CHANNELS, channelCount));
        return;
    }

    auto tempRoot = getAnimationLipSyncTempRoot();
    auto tempDir = tempRoot / jobState.jobId;
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
                failJob(fmt::format("Unable to load creature {}: {}", creatureId,
                                    creatureResult.getError()->getMessage()));
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

        auto extractResult = voice::AudioConverter::extractChannelToMono(
            audioPath, monoPath, config->getFfmpegBinaryPath(), static_cast<int>(creature.audio_channel),
            jobState.span);
        if (!extractResult.isSuccess()) {
            failJob(extractResult.getError()->getMessage());
            return;
        }

        auto lipSyncProgress = [&](float stage) {
            trackStageProgress(0.1 + 0.6 * static_cast<double>(stage));
        };

        auto lipSyncResult =
            voice::LipSyncProcessor::generateLipSync(monoPath.filename().string(), tempDir.string(),
                                                     config->getRhubarbBinaryPath(), true, lipSyncProgress,
                                                     jobState.span);
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

    auto animationJson = animationToJson(animation);
    auto upsertResult = db->upsertAnimation(animationJson.dump(), jobState.span);
    if (!upsertResult.isSuccess()) {
        failJob(upsertResult.getError()->getMessage());
        return;
    }

    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Animation);
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

        auto tempRoot = getAdHocTempRoot();
        auto tempDir = tempRoot / jobState.jobId;
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

        auto speechResult = voice::SpeechGenerationManager::generate(speechRequest);
        if (!speechResult.isSuccess()) {
            failJob(speechResult.getError()->getMessage());
            return;
        }
        updateProgress(0.2f);

        auto speechAssets = speechResult.getValue().value();
        auto creatureName = speechAssets.creature.name.empty() ? creatureId : speechAssets.creature.name;
        auto creatureSlug = slugify(creatureName);
        auto textSlug = slugify(text);
        auto timestamp = fmt::format("{:%Y%m%d%H%M%S}", std::chrono::system_clock::now());
        auto baseName = fmt::format("adhoc_{}_{}_{}", creatureSlug, timestamp, textSlug);

        auto renameIfExists = [&](const std::filesystem::path &oldPath, const std::string &extension) {
            if (oldPath.empty() || !std::filesystem::exists(oldPath)) {
                return;
            }
            std::filesystem::path newPath = oldPath.parent_path() / fmt::format("{}.{}", baseName, extension);
            std::error_code renameEc;
            std::filesystem::rename(oldPath, newPath, renameEc);
            if (renameEc) {
                warn("Unable to rename {} to {}: {}", oldPath.string(), newPath.string(), renameEc.message());
                return;
            }
            if (extension == "wav") {
                speechAssets.wavPath = newPath;
                speechAssets.response.sound_file_name = newPath.filename().string();
            } else if (extension == "mp3") {
                speechAssets.mp3Path = newPath;
            } else if (extension == "txt") {
                speechAssets.transcriptPath = newPath;
                speechAssets.response.transcript_file_name = newPath.filename().string();
            }
        };

        renameIfExists(speechAssets.wavPath, "wav");
        renameIfExists(speechAssets.mp3Path, "mp3");
        renameIfExists(speechAssets.transcriptPath, "txt");

        auto cacheFuture = std::async(std::launch::async, [wavPath = speechAssets.wavPath, span = jobState.span]() {
            debug("Starting background cache prewarm for {}", wavPath.string());
            return prewarmAudioCache(wavPath, span);
        });

        auto rhubarbProgress = [&, base = 0.2f, span = 0.3f](float lipSyncProgress) {
            updateProgress(base + span * lipSyncProgress);
        };

        auto lipSyncResult = voice::LipSyncProcessor::generateLipSync(speechAssets.wavPath.filename().string(),
                                                                      tempDir.string(), config->getRhubarbBinaryPath(),
                                                                      true, rhubarbProgress, jobState.span);
        auto cachePrewarmResult = cacheFuture.get();
        if (!cachePrewarmResult.isSuccess()) {
            warn("Audio cache prewarm failed for job {}: {}", jobState.jobId,
                 cachePrewarmResult.getError()->getMessage());
        } else {
            debug("Audio cache prewarm complete for {}", speechAssets.wavPath.string());
        }
        if (!lipSyncResult.isSuccess()) {
            failJob(lipSyncResult.getError()->getMessage());
            return;
        }

        auto rhubarbData = RhubarbSoundData::fromJsonString(lipSyncResult.getValue().value());
        updateProgress(0.55f);

        if (speechAssets.creature.speech_loop_animation_ids.empty()) {
            failJob(fmt::format("Creature {} has no speech_loop_animation_ids configured", creatureId));
            return;
        }

        if (config->getAnimationSchedulerType() != Configuration::AnimationSchedulerType::Cooperative) {
            failJob("Ad-hoc speech requires the cooperative scheduler");
            return;
        }

        std::mt19937 rng(static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        std::uniform_int_distribution<std::size_t> dist(0, speechAssets.creature.speech_loop_animation_ids.size() - 1);
        auto baseAnimationId = speechAssets.creature.speech_loop_animation_ids[dist(rng)];

        auto baseAnimationResult = db->getAnimation(baseAnimationId, jobState.span);
        if (!baseAnimationResult.isSuccess()) {
            failJob(fmt::format("Unable to load base speech loop animation {}: {}", baseAnimationId,
                                baseAnimationResult.getError()->getMessage()));
            return;
        }
        auto baseAnimation = baseAnimationResult.getValue().value();

        auto trackIt = std::find_if(baseAnimation.tracks.begin(), baseAnimation.tracks.end(),
                                    [&](const Track &track) { return track.creature_id == creatureId; });
        if (trackIt == baseAnimation.tracks.end()) {
            failJob(
                fmt::format("Base speech loop animation {} does not contain creature {}", baseAnimationId, creatureId));
            return;
        }
        const auto &baseTrack = *trackIt;

        if (baseTrack.frames.empty()) {
            failJob(fmt::format("Base speech loop track {} has no frames", baseTrack.id));
            return;
        }

        std::vector<std::vector<uint8_t>> decodedFrames;
        decodedFrames.reserve(baseTrack.frames.size());
        for (const auto &frame : baseTrack.frames) {
            decodedFrames.push_back(decodeBase64(frame));
        }

        const auto frameWidth = decodedFrames.front().size();
        const auto mouthSlot = speechAssets.creature.mouth_slot;
        if (mouthSlot >= frameWidth) {
            failJob(fmt::format("Mouth slot {} out of bounds for frame width {}", mouthSlot, frameWidth));
            return;
        }

        uint32_t msPerFrame = baseAnimation.metadata.milliseconds_per_frame;
        if (msPerFrame == 0) {
            msPerFrame = 1;
        }

        size_t targetFrames = std::max<size_t>(
            1,
            static_cast<size_t>(std::ceil((rhubarbData.metadata.duration * 1000.0) / static_cast<double>(msPerFrame))));

        SoundDataProcessor processor;
        auto mouthData = processor.processSoundData(rhubarbData, msPerFrame, targetFrames);

        std::vector<std::string> encodedFrames;
        encodedFrames.reserve(targetFrames);
        for (size_t idx = 0; idx < targetFrames; ++idx) {
            auto frameData = decodedFrames[idx % decodedFrames.size()];
            frameData[mouthSlot] = mouthData[idx];
            std::string raw(reinterpret_cast<const char *>(frameData.data()), frameData.size());
            encodedFrames.push_back(base64::to_base64(raw));
        }

        Animation adHocAnimation = baseAnimation;
        adHocAnimation.id = util::generateUUID();
        adHocAnimation.metadata.animation_id = adHocAnimation.id;
        adHocAnimation.metadata.title = fmt::format("{} - {} - {}", creatureName, timestamp, textSlug);
        adHocAnimation.metadata.sound_file = speechAssets.wavPath.string();
        adHocAnimation.metadata.note = fmt::format("Ad-hoc speech generated from text: {}", text);
        adHocAnimation.metadata.number_of_frames = static_cast<uint32_t>(encodedFrames.size());
        adHocAnimation.metadata.multitrack_audio = true;

        Track newTrack;
        newTrack.id = util::generateUUID();
        newTrack.creature_id = creatureId;
        newTrack.animation_id = adHocAnimation.id;
        newTrack.frames = std::move(encodedFrames);
        adHocAnimation.tracks = {newTrack};

        auto createdAt = std::chrono::system_clock::now();
        auto insertResult = db->insertAdHocAnimation(adHocAnimation, createdAt, jobState.span);
        if (!insertResult.isSuccess()) {
            failJob(insertResult.getError()->getMessage());
            return;
        }
        scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::AdHocAnimationList);
        scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::AdHocSoundList);
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

} // namespace creatures::jobs
