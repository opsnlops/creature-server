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

} // namespace

LocalSdlAudioTransport::LocalSdlAudioTransport() = default;

LocalSdlAudioTransport::~LocalSdlAudioTransport() { stop(); }

Result<void> LocalSdlAudioTransport::start(std::shared_ptr<PlaybackSession> session) {
    if (!session) {
        return Result<void>{ServerError(ServerError::InvalidData, "No playback session provided")};
    }
    if (!config) {
        return Result<void>{ServerError(ServerError::InternalError, "Audio configuration unavailable")};
    }
    if (!localAudioPlaybackCoordinator) {
        return Result<void>{ServerError(ServerError::InternalError, "Local audio coordinator unavailable")};
    }

    const auto &animation = session->getAnimation();
    if (animation.metadata.sound_file.empty()) {
        return Result<void>{ServerError(ServerError::InvalidData, "No sound file in animation")};
    }

    std::filesystem::path soundFilePath = creatures::storage::resolveSoundPath(animation.metadata.sound_file);
    std::shared_ptr<OperationSpan> playbackSpan;
    if (observability) {
        playbackSpan = observability->createOperationSpan("audio.local.playback");
        playbackSpan->setAttribute("audio.local.mode", "main");
        playbackSpan->setAttribute("audio.local.source", "animation");
        playbackSpan->setAttribute("audio.local.file_name", soundFilePath.filename().string());
        playbackSpan->setAttribute("session.id", session->getSessionId());
        playbackSpan->setAttribute("animation.id", animation.id);
        playbackSpan->setAttribute("session.universe", static_cast<int64_t>(session->getUniverse()));
        if (const auto triggerSpan = session->getSpan()) {
            playbackSpan->setAttribute("trigger.trace_id", triggerSpan->getTraceIdHex());
            playbackSpan->setAttribute("trigger.span_id", triggerSpan->getSpanIdHex());
        }
    }

    auto submission = localAudioPlaybackCoordinator->submit(
        {.id = "animation:" + session->getSessionId(),
         .source = "animation",
         .fileName = soundFilePath.filename().string(),
         .play = [filePath = soundFilePath.string()](
                     const std::atomic<bool> &stopRequested) { return playFileBlocking(filePath, stopRequested); },
         .onFinished =
             [playbackSpan](const audio::LocalAudioPlaybackCoordinator::Completion &completion) {
                 if (!playbackSpan) {
                     return;
                 }
                 playbackSpan->setAttribute("audio.local.generation", static_cast<int64_t>(completion.generation));
                 playbackSpan->setAttribute("audio.local.outcome", audio::LocalAudioPlaybackCoordinator::outcomeName(
                                                                       completion.result.outcome));
                 if (completion.result.outcome == audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed ||
                     completion.result.outcome == audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::TimedOut) {
                     playbackSpan->setAttribute("error.code", completion.result.errorCode);
                     playbackSpan->setAttribute("error.message", completion.result.errorMessage);
                     playbackSpan->setAttribute("audio.local.failure_stage", "playback");
                     playbackSpan->setError(completion.result.errorMessage);
                 } else {
                     playbackSpan->setSuccess();
                 }
                 playbackSpan->end();
             }});
    if (submission.result != audio::LocalAudioPlaybackCoordinator::SubmitResult::Accepted) {
        if (playbackSpan) {
            playbackSpan->setAttribute("error.code", "local_audio.admission_rejected");
            playbackSpan->setAttribute("audio.local.failure_stage", "admission");
            playbackSpan->setError("Local audio coordinator is shutting down");
            playbackSpan->end();
        }
        return Result<void>{ServerError(ServerError::Conflict, "Local audio coordinator is shutting down")};
    }
    playbackHandle_ = std::move(submission.handle);

    debug("LocalSdlAudioTransport submitted generation {} for file: {}", playbackHandle_->generation(),
          soundFilePath.string());

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

        if (Mix_OpenAudioDevice(localAudioDeviceAudioSpec.freq, localAudioDeviceAudioSpec.format,
                                localAudioDeviceAudioSpec.channels, SOUND_BUFFER_SIZE, audioDevice, 1) < 0) {
            const std::string errorMsg = fmt::format("Failed to open audio device: {}", Mix_GetError());
            error(errorMsg);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                    .errorCode = "local_audio.device_open_failed",
                    .errorMessage = errorMsg};
        }
        guard.audioDeviceOpen = true;

        if (stopRequested.load(std::memory_order_acquire)) {
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped};
        }

        guard.music = Mix_LoadMUS(filePath.c_str());
        if (!guard.music) {
            const std::string errorMsg = fmt::format("Failed to load music file '{}': {}", filePath, Mix_GetError());
            error(errorMsg);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                    .errorCode = "local_audio.file_load_failed",
                    .errorMessage = errorMsg};
        }

        const double duration = Mix_MusicDuration(guard.music);
        if (duration > 0.0) {
            debug("Music duration: {:.2f} seconds", duration);
        } else {
            warn("Could not determine music duration for: {}", filePath);
        }

        constexpr double MAX_DURATION_SECONDS = 3600.0; // 1 hour max
        if (duration > MAX_DURATION_SECONDS) {
            const std::string errorMsg =
                fmt::format("Music file too long: {:.2f}s (max: {:.2f}s)", duration, MAX_DURATION_SECONDS);
            error(errorMsg);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
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
                .errorCode = "local_audio.playback_exception",
                .errorMessage = errorMsg};
    } catch (...) {
        const std::string errorMsg = "Unknown exception in audio playback thread";
        error(errorMsg);
        return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                .errorCode = "local_audio.unknown_playback_exception",
                .errorMessage = errorMsg};
    }
}

} // namespace creatures
