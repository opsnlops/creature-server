#include "NativeAudioPlaybackService.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <span>
#include <thread>
#include <typeinfo>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "AlsaMixerControl.h"
#include "DecodedAudioStream.h"
#include "MonoWavDownmixer.h"
#include "util/threadName.h"

namespace creatures::audio {
namespace {

constexpr uintmax_t MAX_LOCAL_AUDIO_FILE_SIZE = 1024ULL * 1024ULL * 1024ULL;
constexpr double MAX_LOCAL_AUDIO_DURATION_SECONDS = 3600.0;
constexpr size_t READ_CHUNK_FRAMES = 4096;

LocalAudioPlaybackCoordinator::PlaybackResult failure(std::string type, std::string code, std::string message) {
    return {.outcome = LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
            .errorType = std::move(type),
            .errorCode = std::move(code),
            .errorMessage = std::move(message)};
}

bool hasWavExtension(const std::string &filePath) {
    std::string extension = std::filesystem::path(filePath).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension == ".wav" || extension == ".wave";
}

} // namespace

NativeAudioPlaybackService::~NativeAudioPlaybackService() { shutdown(); }

bool NativeAudioPlaybackService::initialize(NativeAudioConfig config) {
    shutdown();
    config_ = std::move(config);
    output_ = createAudioOutput();
    if (!output_ || !output_->open(config_)) {
        output_.reset();
        return false;
    }
    if (!AlsaMixerControl::applyConfiguredVolume(config_)) {
        output_->close();
        output_.reset();
        return false;
    }
    {
        std::lock_guard lock(outputMutex_);
        if (!restoreKeepaliveLocked()) {
            output_->close();
            output_.reset();
            return false;
        }
    }
    shutdownRequested_.store(false);
    initialized_ = true;
    keepaliveThread_ = std::thread(&NativeAudioPlaybackService::keepaliveLoop, this);
    return true;
}

void NativeAudioPlaybackService::shutdown() {
    shutdownRequested_.store(true);
    if (keepaliveThread_.joinable()) {
        keepaliveThread_.join();
    }
    std::lock_guard lock(outputMutex_);
    if (output_) {
        output_->stopAndClear();
        output_->close();
        output_.reset();
    }
    initialized_ = false;
    playbackActive_.store(false);
}

LocalAudioPlaybackCoordinator::PlaybackResult
NativeAudioPlaybackService::playFile(const std::string &filePath, NativePlaybackMode mode,
                                     const std::atomic<bool> &stopRequested) {
    try {
        if (!initialized_ || !output_) {
            return failure("AudioDeviceUnavailable", "local_audio.device_unavailable",
                           "Native audio output is not initialized");
        }
        if (mode == NativePlaybackMode::Travel && config_.channels != 2) {
            return failure("AudioConfigurationError", "local_audio.travel_requires_stereo",
                           "Travel playback requires a two-channel native output");
        }
        if (stopRequested.load(std::memory_order_acquire)) {
            return {.outcome = LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped};
        }
        const uint64_t underrunsBeforePlayback = underruns();

        std::error_code fileError;
        const auto fileSize = std::filesystem::file_size(filePath, fileError);
        if (fileError || fileSize > MAX_LOCAL_AUDIO_FILE_SIZE) {
            return failure("AudioFileValidationError",
                           fileError ? "local_audio.file_stat_failed" : "local_audio.file_size_exceeded",
                           fileError
                               ? "Unable to inspect local audio file before playback"
                               : fmt::format("Local audio file exceeds {} byte limit", MAX_LOCAL_AUDIO_FILE_SIZE));
        }

        std::shared_ptr<MonoWavStream> travelWav;
        std::shared_ptr<DecodedAudioStream> decoded;
        uint64_t totalFrames = 0;
        if (hasWavExtension(filePath)) {
            auto streamResult = MonoWavStream::open(filePath, {.dialogGainDb = config_.dialogGainDb,
                                                               .bgmGainDb = config_.bgmGainDb,
                                                               .limiterCeilingDb = config_.limiterCeilingDb});
            if (!streamResult.isSuccess()) {
                return failure("AudioFileLoadError", "local_audio.file_load_failed",
                               streamResult.getError()->getMessage());
            }
            travelWav = streamResult.getValue().value();
            if (travelWav->sampleRate() != static_cast<int>(config_.sampleRate)) {
                return failure("AudioSampleRateMismatch", "local_audio.sample_rate_mismatch",
                               fmt::format("Travel WAV is {} Hz, but native output is configured for {} Hz",
                                           travelWav->sampleRate(), config_.sampleRate));
            }
            totalFrames = travelWav->totalFrames();
        } else {
            auto streamResult = DecodedAudioStream::open(filePath, config_.sampleRate, config_.channels);
            if (!streamResult.isSuccess()) {
                return failure("AudioFileLoadError", "local_audio.file_load_failed",
                               streamResult.getError()->getMessage());
            }
            decoded = streamResult.getValue().value();
            totalFrames = decoded->totalFrames();
        }

        const double durationSeconds =
            totalFrames == 0 ? 0.0 : static_cast<double>(totalFrames) / static_cast<double>(config_.sampleRate);
        if (durationSeconds > MAX_LOCAL_AUDIO_DURATION_SECONDS) {
            return failure("AudioDurationExceeded", "local_audio.duration_exceeded",
                           fmt::format("Local audio duration {:.2f}s exceeds {:.2f}s limit", durationSeconds,
                                       MAX_LOCAL_AUDIO_DURATION_SECONDS));
        }

        playbackActive_.store(true, std::memory_order_release);
        {
            std::lock_guard lock(outputMutex_);
            output_->stopAndClear();
        }

        const uint16_t channels = config_.channels;
        std::vector<int16_t> samples(READ_CHUNK_FRAMES * channels);
        const size_t queueTargetFrames =
            std::max<size_t>(1, static_cast<size_t>(config_.sampleRate) * NATIVE_AUDIO_QUEUE_TARGET_MS / 1000);
        const auto allowedDuration = durationSeconds > 0.0 ? durationSeconds : MAX_LOCAL_AUDIO_DURATION_SECONDS;
        const auto playbackDeadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(static_cast<int64_t>(allowedDuration * 1000.0) + 10000);
        bool reachedEnd = false;
        bool started = false;
        uint64_t framesDecoded = 0;
        const auto withPlaybackStats = [&](LocalAudioPlaybackCoordinator::PlaybackResult result) {
            result.sourceFrames = totalFrames;
            result.decodedFrames = framesDecoded;
            const uint64_t underrunsAfterPlayback = underruns();
            result.deviceUnderruns = underrunsAfterPlayback >= underrunsBeforePlayback
                                         ? underrunsAfterPlayback - underrunsBeforePlayback
                                         : underrunsAfterPlayback;
            return result;
        };
        const uint64_t maximumFrames =
            static_cast<uint64_t>(MAX_LOCAL_AUDIO_DURATION_SECONDS * static_cast<double>(config_.sampleRate));

        while (!stopRequested.load(std::memory_order_acquire)) {
            size_t queuedFrames = 0;
            {
                std::lock_guard lock(outputMutex_);
                queuedFrames = output_->queuedFrames();
            }
            while (!reachedEnd && queuedFrames < queueTargetFrames && !stopRequested.load(std::memory_order_acquire)) {
                Result<size_t> readResult =
                    travelWav ? travelWav->readLocalOutputFrames(samples, channels) : decoded->readFrames(samples);
                if (!readResult.isSuccess()) {
                    finishPlayback();
                    return withPlaybackStats(failure("AudioStreamReadError", "local_audio.stream_read_failed",
                                                     readResult.getError()->getMessage()));
                }
                const size_t framesRead = readResult.getValue().value();
                if (framesRead == 0) {
                    reachedEnd = true;
                    break;
                }
                framesDecoded += framesRead;
                if (framesDecoded > maximumFrames) {
                    finishPlayback();
                    return withPlaybackStats(failure(
                        "AudioDurationExceeded", "local_audio.duration_exceeded",
                        fmt::format("Decoded local audio exceeds {:.2f}s limit", MAX_LOCAL_AUDIO_DURATION_SECONDS)));
                }
                if (!travelWav) {
                    const float gain = std::pow(10.0F, config_.dialogGainDb / 20.0F);
                    const float positiveLimit =
                        static_cast<float>(INT16_MAX) * std::pow(10.0F, config_.limiterCeilingDb / 20.0F);
                    const float negativeLimit =
                        static_cast<float>(INT16_MIN) * std::pow(10.0F, config_.limiterCeilingDb / 20.0F);
                    for (size_t sampleIndex = 0; sampleIndex < framesRead * channels; ++sampleIndex) {
                        samples[sampleIndex] = static_cast<int16_t>(
                            std::clamp(static_cast<float>(samples[sampleIndex]) * gain, negativeLimit, positiveLimit));
                    }
                }
                bool writeFailed = false;
                bool startFailed = false;
                {
                    std::lock_guard lock(outputMutex_);
                    if (!output_->write(std::span<const int16_t>(samples.data(), framesRead * channels))) {
                        writeFailed = true;
                    } else if (!started && !output_->start()) {
                        startFailed = true;
                    } else if (!started) {
                        started = true;
                    }
                    queuedFrames = output_->queuedFrames();
                }
                if (writeFailed || startFailed) {
                    finishPlayback();
                    return withPlaybackStats(
                        failure(writeFailed ? "AudioQueueError" : "AudioDeviceStartError",
                                writeFailed ? "local_audio.queue_failed" : "local_audio.device_start_failed",
                                writeFailed ? "Native audio output rejected decoded frames"
                                            : "Native audio output failed to start"));
                }
            }

            if (reachedEnd) {
                size_t queued = 0;
                size_t pipeline = 0;
                {
                    std::lock_guard lock(outputMutex_);
                    queued = output_->queuedFrames();
                    pipeline = output_->pipelineLatencyFrames();
                }
                if (queued <= pipeline) {
                    if (pipeline > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(static_cast<int64_t>(pipeline) * 1000 / config_.sampleRate));
                    }
                    break;
                }
            }
            if (std::chrono::steady_clock::now() >= playbackDeadline) {
                finishPlayback();
                return withPlaybackStats({.outcome = LocalAudioPlaybackCoordinator::PlaybackOutcome::TimedOut,
                                          .errorType = "AudioPlaybackTimeout",
                                          .errorCode = "local_audio.playback_timeout",
                                          .errorMessage = "Native audio playback exceeded its duration deadline"});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(NATIVE_AUDIO_WAIT_MS));
        }

        if (stopRequested.load(std::memory_order_acquire)) {
            finishPlayback();
            return withPlaybackStats({.outcome = LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped});
        }
        finishPlayback();
        return withPlaybackStats({});
    } catch (const std::exception &exception) {
        finishPlayback();
        return {.outcome = LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                .errorType = typeid(exception).name(),
                .errorCode = "local_audio.playback_exception",
                .errorMessage = fmt::format("Native audio playback failed: {}", exception.what()),
                .exception = std::current_exception()};
    } catch (...) {
        finishPlayback();
        return {.outcome = LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                .errorType = "UnknownPlaybackException",
                .errorCode = "local_audio.unknown_playback_exception",
                .errorMessage = "Native audio playback failed with an unknown exception",
                .exception = std::current_exception()};
    }
}

uint64_t NativeAudioPlaybackService::underruns() const {
    std::lock_guard lock(outputMutex_);
    return output_ ? output_->underruns() : 0;
}

bool NativeAudioPlaybackService::refillKeepaliveLocked() {
    const size_t targetFrames =
        std::max<size_t>(1, static_cast<size_t>(config_.sampleRate) * 20 / 1000) + output_->pipelineLatencyFrames();
    const size_t queued = output_->queuedFrames();
    if (queued >= targetFrames) {
        return true;
    }
    const size_t missingFrames = targetFrames - queued;
    std::vector<int16_t> silence(missingFrames * config_.channels, 0);
    return output_->write(silence);
}

bool NativeAudioPlaybackService::restoreKeepaliveLocked() {
    output_->stopAndClear();
    return refillKeepaliveLocked() && output_->start();
}

void NativeAudioPlaybackService::keepaliveLoop() {
    setThreadName("NativeAudioKeepalive");
    uint64_t consecutiveRefillFailures = 0;
    while (!shutdownRequested_.load(std::memory_order_acquire)) {
        if (!playbackActive_.load(std::memory_order_acquire)) {
            std::lock_guard lock(outputMutex_);
            if (playbackActive_.load(std::memory_order_acquire)) {
                continue;
            }
            if (output_ && !refillKeepaliveLocked()) {
                ++consecutiveRefillFailures;
                if (consecutiveRefillFailures == 1 || consecutiveRefillFailures % 200 == 0) {
                    spdlog::warn("Unable to refill the native audio keepalive queue ({} consecutive failures)",
                                 consecutiveRefillFailures);
                }
            } else if (consecutiveRefillFailures > 0) {
                spdlog::info("Native audio keepalive recovered after {} refill failures", consecutiveRefillFailures);
                consecutiveRefillFailures = 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(NATIVE_AUDIO_WAIT_MS));
    }
}

void NativeAudioPlaybackService::finishPlayback() {
    if (!playbackActive_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard lock(outputMutex_);
    if (output_ && !restoreKeepaliveLocked()) {
        spdlog::error("Unable to restore native audio keepalive after playback");
    }
}

} // namespace creatures::audio
