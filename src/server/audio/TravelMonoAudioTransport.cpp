//
// TravelMonoAudioTransport.cpp
// Mono local playback for travel mode
//

#include "TravelMonoAudioTransport.h"

#include <SDL.h>
#include <chrono>
#include <filesystem>
#include <thread>

#include "MonoWavDownmixer.h"
#include "server/animation/PlaybackSession.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/storage/Storage.h"
#include "spdlog/spdlog.h"

namespace creatures {

extern const char *audioDevice;
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<SystemCounters> metrics;

TravelMonoAudioTransport::TravelMonoAudioTransport() : shouldStop_(false), isPlaying_(false), hasFinished_(false) {}

TravelMonoAudioTransport::~TravelMonoAudioTransport() {
    stop();
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
}

Result<void> TravelMonoAudioTransport::start(std::shared_ptr<PlaybackSession> session) {
    session_ = session;

    if (!session_) {
        return Result<void>{ServerError(ServerError::InvalidData, "No playback session provided")};
    }
    if (!config) {
        return Result<void>{ServerError(ServerError::InternalError, "Audio configuration unavailable")};
    }

    // Get the sound file path from the animation metadata
    const auto &animation = session_->getAnimation();
    if (animation.metadata.sound_file.empty()) {
        return Result<void>{ServerError(ServerError::InvalidData, "No sound file in animation")};
    }

    std::filesystem::path soundFilePath = creatures::storage::resolveSoundPath(animation.metadata.sound_file);

    // Spawn audio thread
    shouldStop_ = false;
    isPlaying_ = true;
    hasFinished_ = false;

    audioThread_ = std::thread(&TravelMonoAudioTransport::audioThreadFunc, this, soundFilePath.string());

    debug("TravelMonoAudioTransport started for file: {}", soundFilePath.string());

    return Result<void>{};
}

void TravelMonoAudioTransport::stop() {
    if (isPlaying_.load()) {
        shouldStop_ = true;
        debug("TravelMonoAudioTransport stop requested");
    }
}

bool TravelMonoAudioTransport::isFinished() const { return hasFinished_.load(); }

void TravelMonoAudioTransport::audioThreadFunc(std::string filePath) {
    playFileBlocking(filePath, &shouldStop_);
    isPlaying_ = false;
    hasFinished_ = true;
}

void TravelMonoAudioTransport::playFileBlocking(const std::string &filePath, const std::atomic<bool> *shouldStop) {

    auto setPlayingSound = [](bool isPlaying) {
        if (gpioPins) {
            gpioPins->playingSound(isPlaying);
        }
    };

    // RAII wrapper for the SDL audio device
    struct SDLDeviceGuard {
        SDL_AudioDeviceID device = 0;
        ~SDLDeviceGuard() {
            if (device != 0) {
                SDL_CloseAudioDevice(device);
            }
        }
    };

    SDLDeviceGuard guard;

    try {
        setPlayingSound(true);

        auto monoResult = audio::loadWavAsMono(filePath);
        if (!monoResult.isSuccess()) {
            error("Travel audio: {}", monoResult.getError()->getMessage());
            setPlayingSound(false);
            return;
        }
        const auto mono = monoResult.getValue().value();

        // Open the device at the file's own rate as mono; SDL converts to whatever
        // the hardware actually wants (e.g. a stereo-only USB dongle).
        SDL_AudioSpec want{};
        want.freq = mono.sampleRate;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = SOUND_BUFFER_SIZE;

        SDL_AudioSpec have{};
        guard.device = SDL_OpenAudioDevice(audioDevice, 0, &want, &have, 0);
        if (guard.device == 0) {
            error("Travel audio: failed to open audio device: {}", SDL_GetError());
            setPlayingSound(false);
            return;
        }

        const auto byteCount = static_cast<Uint32>(mono.samples.size() * sizeof(int16_t));
        if (SDL_QueueAudio(guard.device, mono.samples.data(), byteCount) < 0) {
            error("Travel audio: failed to queue audio: {}", SDL_GetError());
            setPlayingSound(false);
            return;
        }

        SDL_PauseAudioDevice(guard.device, 0);

        const double durationSeconds = static_cast<double>(mono.samples.size()) / mono.sampleRate;
        debug("Travel audio playing: {} ({:.2f} seconds of mono at {} Hz)", filePath, durationSeconds, mono.sampleRate);

        // Wait for the queue to drain, with timeout protection like the other transports
        const int timeoutMs = static_cast<int>(durationSeconds * 1000.0) + 10000;
        int elapsed = 0;

        while (SDL_GetQueuedAudioSize(guard.device) > 0 && elapsed < timeoutMs && !(shouldStop && shouldStop->load())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            elapsed += 100;
        }

        if (shouldStop && shouldStop->load()) {
            debug("Travel audio playback stopped by request");
            SDL_ClearQueuedAudio(guard.device);
        } else if (elapsed >= timeoutMs) {
            warn("Travel audio playback timed out after {} seconds, stopping", timeoutMs / 1000);
            SDL_ClearQueuedAudio(guard.device);
        } else {
            // The queue is empty but the last hardware buffer may still be playing;
            // give it a moment so the tail doesn't get clipped when the device closes
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            debug("Travel audio playback completed successfully");
        }

        if (metrics) {
            metrics->incrementSoundsPlayed();
        }

    } catch (const std::exception &e) {
        error("Exception in travel audio playback: {}", e.what());
    } catch (...) {
        error("Unknown exception in travel audio playback");
    }

    // Device cleanup happens automatically via RAII guard
    setPlayingSound(false);
}

} // namespace creatures
