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
#include <typeinfo>
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

constexpr uintmax_t MAX_LOCAL_AUDIO_FILE_SIZE = 1024ULL * 1024ULL * 1024ULL;
constexpr double MAX_LOCAL_AUDIO_DURATION_SECONDS = 3600.0;

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
        span->setAttribute("audio.local.mode", "travel");
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

TravelMonoAudioTransport::TravelMonoAudioTransport() = default;

TravelMonoAudioTransport::~TravelMonoAudioTransport() { stop(); }

Result<void> TravelMonoAudioTransport::start(std::shared_ptr<PlaybackSession> session) {
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
             .mode = "travel",
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
        const std::string message = fmt::format("Failed to submit travel audio playback: {}", exception.what());
        failAdmissionSpan(admissionSpan, typeid(exception).name(), "local_audio.admission_exception", message,
                          &exception);
        return Result<void>{ServerError(ServerError::InternalError, message)};
    } catch (...) {
        const std::string message = "Failed to submit travel audio playback: unknown exception";
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

    debug("TravelMonoAudioTransport submitted generation {} for file: {}", playbackHandle_->generation(), fileName);

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

        std::error_code fileError;
        const auto fileSize = std::filesystem::file_size(filePath, fileError);
        if (fileError || fileSize > MAX_LOCAL_AUDIO_FILE_SIZE) {
            const std::string errorMessage =
                fileError ? "Unable to inspect travel audio file before playback"
                          : fmt::format("Travel audio file exceeds {} byte limit", MAX_LOCAL_AUDIO_FILE_SIZE);
            error(errorMessage);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                    .errorType = "AudioFileValidationError",
                    .errorCode = fileError ? "local_audio.file_stat_failed" : "local_audio.file_size_exceeded",
                    .errorMessage = errorMessage};
        }

        auto streamResult = audio::MonoWavStream::open(filePath);
        if (!streamResult.isSuccess()) {
            const auto errorMessage = streamResult.getError()->getMessage();
            error("Travel audio: {}", errorMessage);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                    .errorType = "AudioFileLoadError",
                    .errorCode = "local_audio.file_load_failed",
                    .errorMessage = errorMessage};
        }
        const auto stream = streamResult.getValue().value();

        if (stopRequested.load(std::memory_order_acquire)) {
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped};
        }

        const double durationSeconds =
            static_cast<double>(stream->totalFrames()) / static_cast<double>(stream->sampleRate());
        if (durationSeconds > MAX_LOCAL_AUDIO_DURATION_SECONDS) {
            const auto errorMessage = fmt::format("Travel audio duration {:.2f}s exceeds {:.2f}s limit",
                                                  durationSeconds, MAX_LOCAL_AUDIO_DURATION_SECONDS);
            error(errorMessage);
            return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                    .errorType = "AudioDurationExceeded",
                    .errorCode = "local_audio.duration_exceeded",
                    .errorMessage = errorMessage};
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
                    .errorType = "AudioDeviceOpenError",
                    .errorCode = "local_audio.device_open_failed",
                    .errorMessage = errorMessage};
        }

        constexpr size_t READ_CHUNK_FRAMES = 4096;
        constexpr uint32_t QUEUE_TARGET_MS = 500;
        const uint64_t targetQueueBytes =
            static_cast<uint64_t>(stream->sampleRate()) * sizeof(int16_t) * QUEUE_TARGET_MS / 1000;
        std::vector<int16_t> monoChunk(READ_CHUNK_FRAMES);

        const auto playbackDeadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(static_cast<int64_t>(durationSeconds * 1000.0) + 10000);
        bool reachedEndOfFile = false;
        bool playbackStarted = false;
        bool playbackTimedOut = false;

        debug("Travel audio streaming: {} ({:.2f} seconds at {} Hz, queue target {}ms)",
              std::filesystem::path(filePath).filename().string(), durationSeconds, stream->sampleRate(),
              QUEUE_TARGET_MS);

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
                            .errorType = "AudioStreamReadError",
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
                            .errorType = "AudioQueueError",
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
                    .errorType = "AudioPlaybackTimeout",
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
                .errorType = typeid(e).name(),
                .errorCode = "local_audio.playback_exception",
                .errorMessage = errorMessage,
                .exception = std::current_exception()};
    } catch (...) {
        const std::string errorMessage = "Unknown exception in travel audio playback";
        error(errorMessage);
        return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                .errorType = "UnknownPlaybackException",
                .errorCode = "local_audio.unknown_playback_exception",
                .errorMessage = errorMessage,
                .exception = std::current_exception()};
    }
}

} // namespace creatures
