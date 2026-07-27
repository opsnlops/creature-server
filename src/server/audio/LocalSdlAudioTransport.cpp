//
// LocalSdlAudioTransport.cpp
// SDL local playback audio transport implementation
//

#include "LocalSdlAudioTransport.h"

#include <SDL.h>
#include <SDL_mixer.h>
#include <chrono>
#include <filesystem>
#include <thread>
#include <typeinfo>

#include "server/animation/PlaybackSession.h"
#include "server/config/Configuration.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/storage/Storage.h"
#include "spdlog/spdlog.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern const char *audioDevice;
extern std::shared_ptr<Configuration> config;
extern SDL_AudioSpec localAudioDeviceAudioSpec;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<audio::LocalAudioPlaybackCoordinator> localAudioPlaybackCoordinator;
extern std::shared_ptr<ObservabilityManager> observability;

namespace {

constexpr uintmax_t MAX_LOCAL_AUDIO_FILE_SIZE = 1024ULL * 1024ULL * 1024ULL;

struct PlayingSoundGuard {
    PlayingSoundGuard() {
        if (gpioPins) {
            gpioPins->playingSound(true);
        }
    }

    ~PlayingSoundGuard() {
        if (gpioPins) {
            gpioPins->playingSound(false);
        }
    }
};

std::shared_ptr<OperationSpan> createAdmissionSpan(const std::shared_ptr<PlaybackSession> &session) {
    if (!observability) {
        return nullptr;
    }
    const auto triggerSpan = session ? session->getSpan() : nullptr;
    auto span = triggerSpan ? observability->createChildOperationSpan("audio.local.admission", triggerSpan)
                            : observability->createOperationSpan("audio.local.admission");
    if (span) {
        span->setAttribute("audio.local.mode", "main");
        span->setAttribute("audio.local.source", "animation");
        span->setAttribute("audio.local.queue_capacity", static_cast<int64_t>(1));
    }
    return span;
}

void failAdmissionSpan(const std::shared_ptr<OperationSpan> &span, const std::string &errorType,
                       const std::string &errorCode, const std::string &errorMessage,
                       const std::exception *exception = nullptr) {
    if (!span) {
        return;
    }
    span->setAttribute("audio.local.outcome", "rejected");
    span->setAttribute("audio.local.failure_stage", "admission");
    span->setAttribute("error.type", errorType);
    span->setAttribute("error.code", errorCode);
    span->setAttribute("error.message", errorMessage);
    if (exception) {
        span->recordException(*exception);
    }
    span->setError(errorMessage);
    span->end();
}

} // namespace

LocalSdlAudioTransport::LocalSdlAudioTransport() = default;

LocalSdlAudioTransport::~LocalSdlAudioTransport() { stop(); }

Result<void> LocalSdlAudioTransport::start(std::shared_ptr<PlaybackSession> session) {
    auto admissionSpan = createAdmissionSpan(session);
    if (!session) {
        const std::string message = "No playback session provided";
        failAdmissionSpan(admissionSpan, "InvalidPlaybackSession", "local_audio.session_missing", message);
        return Result<void>{ServerError(ServerError::InvalidData, message)};
    }
    if (!config) {
        const std::string message = "Audio configuration unavailable";
        failAdmissionSpan(admissionSpan, "AudioConfigurationUnavailable", "local_audio.config_unavailable", message);
        return Result<void>{ServerError(ServerError::InternalError, message)};
    }
    if (!localAudioPlaybackCoordinator) {
        const std::string message = "Local audio coordinator unavailable";
        failAdmissionSpan(admissionSpan, "AudioCoordinatorUnavailable", "local_audio.coordinator_unavailable", message);
        return Result<void>{ServerError(ServerError::InternalError, message)};
    }

    const auto &animation = session->getAnimation();
    if (animation.metadata.sound_file.empty()) {
        const std::string message = "No sound file in animation";
        failAdmissionSpan(admissionSpan, "MissingAudioFile", "local_audio.file_missing", message);
        return Result<void>{ServerError(ServerError::InvalidData, message)};
    }

    std::filesystem::path soundFilePath = creatures::storage::resolveSoundPath(animation.metadata.sound_file);
    const auto fileName = soundFilePath.filename().string();
    const auto triggerSpan = session->getSpan();
    if (admissionSpan) {
        admissionSpan->setAttribute("audio.file.name", fileName);
        admissionSpan->setAttribute("session.id", session->getSessionId());
        admissionSpan->setAttribute("animation.id", animation.id);
        admissionSpan->setAttribute("session.universe", static_cast<int64_t>(session->getUniverse()));
    }

    audio::LocalAudioPlaybackCoordinator::Submission submission;
    try {
        submission = localAudioPlaybackCoordinator->submit(
            {.id = "animation:" + session->getSessionId(),
             .source = "animation",
             .mode = "main",
             .fileName = fileName,
             .sessionId = session->getSessionId(),
             .animationId = animation.id,
             .universe = session->getUniverse(),
             .triggerTraceId = triggerSpan ? triggerSpan->getTraceIdHex() : std::string{},
             .triggerSpanId = triggerSpan ? triggerSpan->getSpanIdHex() : std::string{},
             .play = [filePath = soundFilePath.string()](const std::atomic<bool> &stopRequested) {
                 return playFileBlocking(filePath, stopRequested);
             }});
    } catch (const std::exception &exception) {
        const std::string message = fmt::format("Failed to submit local audio playback: {}", exception.what());
        failAdmissionSpan(admissionSpan, typeid(exception).name(), "local_audio.admission_exception", message,
                          &exception);
        return Result<void>{ServerError(ServerError::InternalError, message)};
    } catch (...) {
        const std::string message = "Failed to submit local audio playback: unknown exception";
        failAdmissionSpan(admissionSpan, "UnknownAdmissionException", "local_audio.admission_exception", message);
        return Result<void>{ServerError(ServerError::InternalError, message)};
    }
    if (submission.result != audio::LocalAudioPlaybackCoordinator::SubmitResult::Accepted) {
        const std::string message = "Local audio coordinator is shutting down";
        failAdmissionSpan(admissionSpan, "AudioAdmissionRejected", "local_audio.admission_rejected", message);
        return Result<void>{ServerError(ServerError::Conflict, message)};
    }
    playbackHandle_ = std::move(submission.handle);

    if (admissionSpan) {
        admissionSpan->setAttribute("audio.local.generation", static_cast<int64_t>(playbackHandle_->generation()));
        admissionSpan->setAttribute("audio.local.outcome", "accepted");
        admissionSpan->setSuccess();
        admissionSpan->end();
    }

    debug("LocalSdlAudioTransport submitted generation {} for file: {}", playbackHandle_->generation(), fileName);

    return Result<void>{};
}

void LocalSdlAudioTransport::stop() {
    if (playbackHandle_) {
        playbackHandle_->stop();
    }
}

bool LocalSdlAudioTransport::isFinished() const { return playbackHandle_ && playbackHandle_->isFinished(); }

audio::LocalAudioPlaybackCoordinator::PlaybackResult
LocalSdlAudioTransport::playFileBlocking(const std::string &filePath, const std::atomic<bool> &stopRequested) {
    struct SDLMixerGuard {
        Mix_Music *music = nullptr;
        bool audioDeviceOpen = false;

        ~SDLMixerGuard() { cleanup(); }

        void cleanup() {
            if (music) {
                Mix_HaltMusic();
                Mix_FreeMusic(music);
                music = nullptr;
            }
            if (audioDeviceOpen) {
                Mix_CloseAudio();
                audioDeviceOpen = false;
            }
        }
    };

    PlayingSoundGuard playingSoundGuard;
    SDLMixerGuard guard;

    try {
        if (stopRequested.load(std::memory_order_acquire)) {
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped};
        }

        std::error_code fileError;
        const auto fileSize = std::filesystem::file_size(filePath, fileError);
        if (fileError || fileSize > MAX_LOCAL_AUDIO_FILE_SIZE) {
            const std::string errorMsg =
                fileError ? "Unable to inspect local audio file before playback"
                          : fmt::format("Local audio file exceeds {} byte limit", MAX_LOCAL_AUDIO_FILE_SIZE);
            error(errorMsg);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                    .errorType = "AudioFileValidationError",
                    .errorCode = fileError ? "local_audio.file_stat_failed" : "local_audio.file_size_exceeded",
                    .errorMessage = errorMsg};
        }

        if (Mix_OpenAudioDevice(localAudioDeviceAudioSpec.freq, localAudioDeviceAudioSpec.format,
                                localAudioDeviceAudioSpec.channels, SOUND_BUFFER_SIZE, audioDevice, 1) < 0) {
            const std::string errorMsg = fmt::format("Failed to open audio device: {}", Mix_GetError());
            error(errorMsg);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                    .errorType = "AudioDeviceOpenError",
                    .errorCode = "local_audio.device_open_failed",
                    .errorMessage = errorMsg};
        }
        guard.audioDeviceOpen = true;

        if (stopRequested.load(std::memory_order_acquire)) {
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped};
        }

        guard.music = Mix_LoadMUS(filePath.c_str());
        if (!guard.music) {
            const std::string errorMsg =
                fmt::format("Failed to load music file '{}': {}", std::filesystem::path(filePath).filename().string(),
                            Mix_GetError());
            error(errorMsg);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                    .errorType = "AudioFileLoadError",
                    .errorCode = "local_audio.file_load_failed",
                    .errorMessage = errorMsg};
        }

        const double duration = Mix_MusicDuration(guard.music);
        if (duration > 0.0) {
            debug("Music duration: {:.2f} seconds", duration);
        } else {
            warn("Could not determine music duration for: {}", std::filesystem::path(filePath).filename().string());
        }

        constexpr double MAX_DURATION_SECONDS = 3600.0; // 1 hour max
        if (duration > MAX_DURATION_SECONDS) {
            const std::string errorMsg =
                fmt::format("Music file too long: {:.2f}s (max: {:.2f}s)", duration, MAX_DURATION_SECONDS);
            error(errorMsg);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                    .errorType = "AudioDurationExceeded",
                    .errorCode = "local_audio.duration_exceeded",
                    .errorMessage = errorMsg};
        }

        if (stopRequested.load(std::memory_order_acquire)) {
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped};
        }

        if (Mix_PlayMusic(guard.music, 1) == -1) {
            const std::string errorMsg = fmt::format("Failed to start music playback: {}", Mix_GetError());
            error(errorMsg);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                    .errorType = "AudioPlaybackStartError",
                    .errorCode = "local_audio.playback_start_failed",
                    .errorMessage = errorMsg};
        }

        constexpr int TIMEOUT_MS = static_cast<int>(MAX_DURATION_SECONDS * 1000) + 10000; // Duration + 10s buffer
        int elapsed = 0;
        constexpr int STOP_POLL_MS = 20;
        while (Mix_PlayingMusic() && elapsed < TIMEOUT_MS && !stopRequested.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(STOP_POLL_MS));
            elapsed += STOP_POLL_MS;
        }

        if (stopRequested.load(std::memory_order_acquire)) {
            debug("Audio playback stopped by request");
            Mix_HaltMusic();
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped};
        }
        if (elapsed >= TIMEOUT_MS) {
            warn("Audio playback timed out after {} seconds, stopping", TIMEOUT_MS / 1000);
            Mix_HaltMusic();
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::TimedOut,
                    .errorType = "AudioPlaybackTimeout",
                    .errorCode = "local_audio.playback_timeout",
                    .errorMessage = fmt::format("Audio playback timed out after {} seconds", TIMEOUT_MS / 1000)};
        }

        debug("Local audio playback completed successfully");
        if (metrics) {
            metrics->incrementSoundsPlayed();
        }
        return {};
    } catch (const std::exception &e) {
        const std::string errorMsg = fmt::format("Exception in audio playback thread: {}", e.what());
        error(errorMsg);
        return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                .errorType = typeid(e).name(),
                .errorCode = "local_audio.playback_exception",
                .errorMessage = errorMsg,
                .exception = std::current_exception()};
    } catch (...) {
        const std::string errorMsg = "Unknown exception in audio playback thread";
        error(errorMsg);
        return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                .errorType = "UnknownPlaybackException",
                .errorCode = "local_audio.unknown_playback_exception",
                .errorMessage = errorMsg,
                .exception = std::current_exception()};
    }
}

} // namespace creatures
