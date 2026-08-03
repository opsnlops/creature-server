#include "LocalNativeAudioTransport.h"

#include <filesystem>
#include <typeinfo>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "server/animation/PlaybackSession.h"
#include "server/config/Configuration.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/storage/Storage.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<audio::LocalAudioPlaybackCoordinator> localAudioPlaybackCoordinator;
extern std::shared_ptr<audio::NativeAudioPlaybackService> nativeAudioPlaybackService;
extern std::shared_ptr<ObservabilityManager> observability;

namespace {

const char *modeName(audio::NativePlaybackMode mode) {
    return mode == audio::NativePlaybackMode::Travel ? "travel" : "main";
}

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

std::shared_ptr<OperationSpan> createAdmissionSpan(const std::shared_ptr<PlaybackSession> &session,
                                                   audio::NativePlaybackMode mode) {
    if (!observability) {
        return nullptr;
    }
    const auto triggerSpan = session ? session->getSpan() : nullptr;
    auto span = triggerSpan ? observability->createChildOperationSpan("audio.local.admission", triggerSpan)
                            : observability->createOperationSpan("audio.local.admission");
    if (span) {
        span->setAttribute("audio.local.mode", modeName(mode));
        span->setAttribute("audio.local.backend", audio::nativeAudioBackendName());
        span->setAttribute("audio.local.source", "animation");
        span->setAttribute("audio.local.queue_capacity", static_cast<int64_t>(1));
        if (config) {
            span->setAttribute("audio.device.name", config->getSoundDeviceName().value_or("default"));
            span->setAttribute("audio.output.sample_rate_hz", static_cast<int64_t>(config->getSoundFrequency()));
            span->setAttribute("audio.output.channels", static_cast<int64_t>(config->getSoundChannels()));
        }
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

LocalNativeAudioTransport::LocalNativeAudioTransport(audio::NativePlaybackMode mode) : mode_(mode) {}
LocalNativeAudioTransport::~LocalNativeAudioTransport() { stop(); }

Result<void> LocalNativeAudioTransport::start(std::shared_ptr<PlaybackSession> session) {
    auto admissionSpan = createAdmissionSpan(session, mode_);
    if (!session || !localAudioPlaybackCoordinator || !nativeAudioPlaybackService) {
        const std::string message = !session                         ? "No playback session provided"
                                    : !localAudioPlaybackCoordinator ? "Local audio coordinator unavailable"
                                                                     : "Native audio service unavailable";
        failAdmissionSpan(admissionSpan, "AudioAdmissionUnavailable", "local_audio.admission_unavailable", message);
        return Result<void>{ServerError(ServerError::InternalError, message)};
    }

    const auto &animation = session->getAnimation();
    if (animation.metadata.sound_file.empty()) {
        const std::string message = "No sound file in animation";
        failAdmissionSpan(admissionSpan, "MissingAudioFile", "local_audio.file_missing", message);
        return Result<void>{ServerError(ServerError::InvalidData, message)};
    }
    const std::filesystem::path soundFilePath = storage::resolveSoundPath(animation.metadata.sound_file);
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
             .mode = modeName(mode_),
             .backend = audio::nativeAudioBackendName(),
             .deviceName = config->getSoundDeviceName().value_or("default"),
             .outputSampleRate = config->getSoundFrequency(),
             .outputChannels = config->getSoundChannels(),
             .fileName = fileName,
             .sessionId = session->getSessionId(),
             .animationId = animation.id,
             .universe = session->getUniverse(),
             .triggerTraceId = triggerSpan ? triggerSpan->getTraceIdHex() : std::string{},
             .triggerSpanId = triggerSpan ? triggerSpan->getSpanIdHex() : std::string{},
             .play = [filePath = soundFilePath.string(), mode = mode_](const std::atomic<bool> &stopRequested) {
                 return playFileBlocking(filePath, mode, stopRequested);
             }});
    } catch (const std::exception &exception) {
        const std::string message = fmt::format("Failed to submit native audio playback: {}", exception.what());
        failAdmissionSpan(admissionSpan, typeid(exception).name(), "local_audio.admission_exception", message,
                          &exception);
        return Result<void>{ServerError(ServerError::InternalError, message)};
    } catch (...) {
        const std::string message = "Failed to submit native audio playback due to an unknown exception";
        failAdmissionSpan(admissionSpan, "UnknownAdmissionException", "local_audio.unknown_admission_exception",
                          message);
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
    spdlog::debug("Native audio submitted generation {} for file '{}'", playbackHandle_->generation(), fileName);
    return Result<void>{};
}

void LocalNativeAudioTransport::stop() {
    if (playbackHandle_) {
        playbackHandle_->stop();
    }
}

bool LocalNativeAudioTransport::isFinished() const { return playbackHandle_ && playbackHandle_->isFinished(); }

audio::LocalAudioPlaybackCoordinator::PlaybackResult
LocalNativeAudioTransport::playFileBlocking(const std::string &filePath, audio::NativePlaybackMode mode,
                                            const std::atomic<bool> &stopRequested) {
    PlayingSoundGuard playingSoundGuard;
    if (!nativeAudioPlaybackService) {
        return {.outcome = audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                .errorType = "AudioServiceUnavailable",
                .errorCode = "local_audio.service_unavailable",
                .errorMessage = "Native audio playback service is unavailable"};
    }
    auto result = nativeAudioPlaybackService->playFile(filePath, mode, stopRequested);
    if (result.outcome == audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Completed && metrics) {
        metrics->incrementSoundsPlayed();
    }
    return result;
}

} // namespace creatures
