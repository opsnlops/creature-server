//
// TravelMonoAudioTransport.cpp
// Mono local playback for travel mode
//

#include "TravelMonoAudioTransport.h"

#include <SDL.h>
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

        auto streamResult = audio::MonoWavStream::open(filePath);
        if (!streamResult.isSuccess()) {
            error("Travel audio: {}", streamResult.getError()->getMessage());
            setPlayingSound(false);
            return;
        }
        const auto stream = streamResult.getValue().value();

        // Open the device at the file's own rate as mono; SDL converts to whatever
        // the hardware actually wants (e.g. a stereo-only USB dongle).
        SDL_AudioSpec want{};
        want.freq = stream->sampleRate();
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

        while (!(shouldStop && shouldStop->load())) {
            uint64_t queuedBytes = SDL_GetQueuedAudioSize(guard.device);
            while (!reachedEndOfFile && queuedBytes < targetQueueBytes) {
                auto readResult = stream->readMonoFrames(std::span<int16_t>{monoChunk});
                if (!readResult.isSuccess()) {
                    error("Travel audio: {}", readResult.getError()->getMessage());
                    setPlayingSound(false);
                    return;
                }

                const size_t framesRead = readResult.getValue().value();
                if (framesRead == 0) {
                    reachedEndOfFile = true;
                    break;
                }

                const auto byteCount = static_cast<Uint32>(framesRead * sizeof(int16_t));
                if (SDL_QueueAudio(guard.device, monoChunk.data(), byteCount) < 0) {
                    error("Travel audio: failed to queue audio: {}", SDL_GetError());
                    setPlayingSound(false);
                    return;
                }
                queuedBytes += byteCount;
            }

            if (!playbackStarted && queuedBytes > 0) {
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

        if (shouldStop && shouldStop->load()) {
            debug("Travel audio playback stopped by request");
            SDL_ClearQueuedAudio(guard.device);
        } else if (playbackTimedOut) {
            debug("Travel audio playback ended after timeout");
        } else {
            // The queue is empty but the last hardware buffer may still be playing;
            // give it a moment so the tail doesn't get clipped when the device closes
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            debug("Travel audio playback completed successfully");

            if (metrics) {
                metrics->incrementSoundsPlayed();
            }
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
