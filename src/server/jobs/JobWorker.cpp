
#include "JobWorker.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>

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
} // namespace creatures

namespace creatures::jobs {

namespace {

std::filesystem::path getAdHocTempRoot() { return std::filesystem::temp_directory_path() / "creature-adhoc"; }

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

    try {
        auto detailsJson = nlohmann::json::parse(jobState.details);
        creatureId = detailsJson.at("creature_id").get<std::string>();
        text = detailsJson.at("text").get<std::string>();
        resumePlaylist = detailsJson.value("resume_playlist", true);
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

        auto rhubarbProgress = [&, base = 0.2f, span = 0.3f](float lipSyncProgress) {
            updateProgress(base + span * lipSyncProgress);
        };

        auto lipSyncResult = voice::LipSyncProcessor::generateLipSync(speechAssets.wavPath.filename().string(),
                                                                      tempDir.string(), config->getRhubarbBinaryPath(),
                                                                      true, rhubarbProgress, jobState.span);
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

        universe_t universe;
        try {
            auto universePtr = creatureUniverseMap->get(creatureId);
            universe = *universePtr;
        } catch (const std::exception &) {
            failJob(
                fmt::format("Creature {} is not registered with a universe. Is the controller online?", creatureId));
            return;
        }

        auto sessionResult = sessionManager->interrupt(universe, adHocAnimation, resumePlaylist);
        if (!sessionResult.isSuccess()) {
            failJob(sessionResult.getError()->getMessage());
            return;
        }

        nlohmann::json completionJson;
        completionJson["animation_id"] = adHocAnimation.id;
        completionJson["sound_file"] = adHocAnimation.metadata.sound_file;
        completionJson["universe"] = universe;
        completionJson["resume_playlist"] = resumePlaylist;
        completionJson["temp_directory"] = tempDir.string();

        updateProgress(1.0f);
        jobManager_->completeJob(jobState.jobId, completionJson.dump());
        broadcastCompletion(jobState.jobId);
        info("Ad-hoc job {} completed successfully", jobState.jobId);

    } catch (const std::exception &e) {
        failJob(e.what());
    }
}

} // namespace creatures::jobs
