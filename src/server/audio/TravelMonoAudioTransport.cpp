//
// TravelMonoAudioTransport.cpp
// Mono local playback for travel mode
//

#include "TravelMonoAudioTransport.h"

#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <span>
#include <thread>
#include <vector>

#include "MonoWavDownmixer.h"
#include "server/animation/PlaybackSession.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/storage/Storage.h"
#include "spdlog/spdlog.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern const char *audioDevice;
extern std::shared_ptr<Configuration> config;
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

TravelMonoAudioTransport::TravelMonoAudioTransport() = default;

TravelMonoAudioTransport::~TravelMonoAudioTransport() { stop(); }

Result<void> TravelMonoAudioTransport::start(std::shared_ptr<PlaybackSession> session) {
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
        playbackSpan->setAttribute("audio.local.mode", "travel");
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

    debug("TravelMonoAudioTransport submitted generation {} for file: {}", playbackHandle_->generation(),
          soundFilePath.string());

    return Result<void>{};
}

void TravelMonoAudioTransport::stop() {
    if (playbackHandle_) {
        playbackHandle_->stop();
    }
}

bool TravelMonoAudioTransport::isFinished() const { return playbackHandle_ && playbackHandle_->isFinished(); }

audio::LocalAudioPlaybackCoordinator::PlaybackResult
TravelMonoAudioTransport::playFileBlocking(const std::string &filePath, const std::atomic<bool> &stopRequested) {
    struct SDLDeviceGuard {
        SDL_AudioDeviceID device = 0;
        ~SDLDeviceGuard() {
            if (device != 0) {
                SDL_CloseAudioDevice(device);
            }
        }
    };

    PlayingSoundGuard playingSoundGuard;
    SDLDeviceGuard guard;

    try {
        if (stopRequested.load(std::memory_order_acquire)) {
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped};
        }

        auto streamResult = audio::MonoWavStream::open(filePath);
        if (!streamResult.isSuccess()) {
            const auto errorMessage = streamResult.getError()->getMessage();
            error("Travel audio: {}", errorMessage);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                    .errorCode = "local_audio.file_load_failed",
                    .errorMessage = errorMessage};
        }
        const auto stream = streamResult.getValue().value();

        if (stopRequested.load(std::memory_order_acquire)) {
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped};
        }

        SDL_AudioSpec want{};
        want.freq = stream->sampleRate();
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = SOUND_BUFFER_SIZE;

        SDL_AudioSpec have{};
        guard.device = SDL_OpenAudioDevice(audioDevice, 0, &want, &have, 0);
        if (guard.device == 0) {
            const auto errorMessage = fmt::format("Travel audio: failed to open audio device: {}", SDL_GetError());
            error(errorMessage);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                    .errorCode = "local_audio.device_open_failed",
                    .errorMessage = errorMessage};
        }

        constexpr size_t READ_CHUNK_FRAMES = 4096;
        constexpr uint32_t QUEUE_TARGET_MS = 500;
        const uint64_t targetQueueBytes =
            static_cast<uint64_t>(stream->sampleRate()) * sizeof(int16_t) * QUEUE_TARGET_MS / 1000;
        std::vector<int16_t> monoChunk(READ_CHUNK_FRAMES);

        const double durationSeconds =
            static_cast<double>(stream->totalFrames()) / static_cast<double>(stream->sampleRate());
        const auto playbackDeadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(static_cast<int64_t>(durationSeconds * 1000.0) + 10000);
        bool reachedEndOfFile = false;
        bool playbackStarted = false;
        bool playbackTimedOut = false;

        debug("Travel audio streaming: {} ({:.2f} seconds at {} Hz, queue target {}ms)", filePath, durationSeconds,
              stream->sampleRate(), QUEUE_TARGET_MS);

        while (!stopRequested.load(std::memory_order_acquire)) {
            uint64_t queuedBytes = SDL_GetQueuedAudioSize(guard.device);
            while (!reachedEndOfFile && queuedBytes < targetQueueBytes) {
                if (stopRequested.load(std::memory_order_acquire)) {
                    break;
                }
                auto readResult = stream->readMonoFrames(std::span<int16_t>{monoChunk});
                if (!readResult.isSuccess()) {
                    const auto errorMessage = readResult.getError()->getMessage();
                    error("Travel audio: {}", errorMessage);
                    return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                            .errorCode = "local_audio.stream_read_failed",
                            .errorMessage = errorMessage};
                }

                const size_t framesRead = readResult.getValue().value();
                if (framesRead == 0) {
                    reachedEndOfFile = true;
                    break;
                }

                const auto byteCount = static_cast<Uint32>(framesRead * sizeof(int16_t));
                if (SDL_QueueAudio(guard.device, monoChunk.data(), byteCount) < 0) {
                    const auto errorMessage = fmt::format("Travel audio: failed to queue audio: {}", SDL_GetError());
                    error(errorMessage);
                    return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                            .errorCode = "local_audio.queue_failed",
                            .errorMessage = errorMessage};
                }
                queuedBytes += byteCount;
            }

            if (!playbackStarted && queuedBytes > 0 && !stopRequested.load(std::memory_order_acquire)) {
                SDL_PauseAudioDevice(guard.device, 0);
                playbackStarted = true;
            }
            if (reachedEndOfFile && SDL_GetQueuedAudioSize(guard.device) == 0) {
                break;
            }
            if (std::chrono::steady_clock::now() >= playbackDeadline) {
                warn("Travel audio playback timed out after {:.2f} seconds, stopping", durationSeconds + 10.0);
                SDL_ClearQueuedAudio(guard.device);
                playbackTimedOut = true;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (stopRequested.load(std::memory_order_acquire)) {
            debug("Travel audio playback stopped by request");
            SDL_ClearQueuedAudio(guard.device);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped};
        }
        if (playbackTimedOut) {
            debug("Travel audio playback ended after timeout");
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::TimedOut,
                    .errorCode = "local_audio.playback_timeout",
                    .errorMessage =
                        fmt::format("Travel audio playback timed out after {:.2f} seconds", durationSeconds + 10.0)};
        }

        // The SDL queue is empty but the last hardware buffer may still be
        // playing. Keep the tail without making replacement wait 250 ms.
        constexpr auto TAIL_DRAIN_TIME = std::chrono::milliseconds(250);
        constexpr auto STOP_POLL_TIME = std::chrono::milliseconds(10);
        auto remainingDrainTime = TAIL_DRAIN_TIME;
        while (remainingDrainTime > std::chrono::milliseconds::zero() &&
               !stopRequested.load(std::memory_order_acquire)) {
            const auto sleepTime = std::min(remainingDrainTime, STOP_POLL_TIME);
            std::this_thread::sleep_for(sleepTime);
            remainingDrainTime -= sleepTime;
        }
        if (stopRequested.load(std::memory_order_acquire)) {
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped};
        }

        debug("Travel audio playback completed successfully");
        if (metrics) {
            metrics->incrementSoundsPlayed();
        }
        return {};

    } catch (const std::exception &e) {
        const auto errorMessage = fmt::format("Exception in travel audio playback: {}", e.what());
        error(errorMessage);
        return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                .errorCode = "local_audio.playback_exception",
                .errorMessage = errorMessage};
    } catch (...) {
        const std::string errorMessage = "Unknown exception in travel audio playback";
        error(errorMessage);
        return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                .errorCode = "local_audio.unknown_playback_exception",
                .errorMessage = errorMessage};
    }
}

} // namespace creatures
