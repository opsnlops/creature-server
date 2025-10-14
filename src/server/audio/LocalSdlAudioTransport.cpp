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
#include "spdlog/spdlog.h"

namespace creatures {

extern const char *audioDevice;
extern SDL_AudioSpec localAudioDeviceAudioSpec;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<SystemCounters> metrics;

LocalSdlAudioTransport::LocalSdlAudioTransport() : shouldStop_(false), isPlaying_(false), hasFinished_(false) {}

LocalSdlAudioTransport::~LocalSdlAudioTransport() {
    stop();
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
}

Result<void> LocalSdlAudioTransport::start(std::shared_ptr<PlaybackSession> session) {
    session_ = session;

    // Get the sound file path from the animation metadata
    const auto &animation = session_->getAnimation();
    if (animation.metadata.sound_file.empty()) {
        return Result<void>{ServerError(ServerError::InvalidData, "No sound file in animation")};
    }

    // Build full path (config will be accessed in thread)
    std::string soundFilePath = animation.metadata.sound_file;

    // Spawn audio thread
    shouldStop_ = false;
    isPlaying_ = true;
    hasFinished_ = false;

    audioThread_ = std::thread(&LocalSdlAudioTransport::audioThreadFunc, this, soundFilePath);

    debug("LocalSdlAudioTransport started for file: {}", soundFilePath);

    return Result<void>{};
}

void LocalSdlAudioTransport::stop() {
    if (isPlaying_.load()) {
        shouldStop_ = true;
        debug("LocalSdlAudioTransport stop requested");
    }
}

bool LocalSdlAudioTransport::isFinished() const { return hasFinished_.load(); }

void LocalSdlAudioTransport::audioThreadFunc(std::string filePath) {
    // RAII wrapper for SDL Mixer resources
    struct SDLMixerGuard {
        Mix_Music *music = nullptr;
        bool audioDeviceOpen = false;

        ~SDLMixerGuard() { cleanup(); }

        void cleanup() {
            if (music) {
                Mix_FreeMusic(music);
                music = nullptr;
            }
            if (audioDeviceOpen) {
                Mix_CloseAudio();
                audioDeviceOpen = false;
            }
        }
    };

    SDLMixerGuard guard;

    try {
        gpioPins->playingSound(true);

        // Open audio device with error checking
        if (Mix_OpenAudioDevice(localAudioDeviceAudioSpec.freq, localAudioDeviceAudioSpec.format,
                                localAudioDeviceAudioSpec.channels, SOUND_BUFFER_SIZE, audioDevice, 1) < 0) {
            const std::string errorMsg = fmt::format("Failed to open audio device: {}", Mix_GetError());
            error(errorMsg);
            isPlaying_ = false;
            hasFinished_ = true;
            gpioPins->playingSound(false);
            return;
        }
        guard.audioDeviceOpen = true;

        // Load music file with validation
        guard.music = Mix_LoadMUS(filePath.c_str());
        if (!guard.music) {
            const std::string errorMsg = fmt::format("Failed to load music file '{}': {}", filePath, Mix_GetError());
            error(errorMsg);
            isPlaying_ = false;
            hasFinished_ = true;
            gpioPins->playingSound(false);
            return;
        }

        // Get duration safely
        double duration = Mix_MusicDuration(guard.music);
        if (duration > 0.0) {
            debug("Music duration: {:.2f} seconds", duration);
        } else {
            warn("Could not determine music duration for: {}", filePath);
        }

        // Validate duration is reasonable (prevent infinite loops)
        constexpr double MAX_DURATION_SECONDS = 3600.0; // 1 hour max
        if (duration > MAX_DURATION_SECONDS) {
            const std::string errorMsg =
                fmt::format("Music file too long: {:.2f}s (max: {:.2f}s)", duration, MAX_DURATION_SECONDS);
            error(errorMsg);
            isPlaying_ = false;
            hasFinished_ = true;
            gpioPins->playingSound(false);
            return;
        }

        // Start playback
        if (Mix_PlayMusic(guard.music, 1) == -1) {
            const std::string errorMsg = fmt::format("Failed to start music playback: {}", Mix_GetError());
            error(errorMsg);
            isPlaying_ = false;
            hasFinished_ = true;
            gpioPins->playingSound(false);
            return;
        }

        // Wait for playback to finish with timeout protection and stop check
        constexpr int TIMEOUT_MS = static_cast<int>(MAX_DURATION_SECONDS * 1000) + 10000; // Duration + 10s buffer
        int elapsed = 0;

        while (Mix_PlayingMusic() && elapsed < TIMEOUT_MS && !shouldStop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            elapsed += 100;
        }

        // Check why we exited
        if (shouldStop_.load()) {
            debug("Audio playback stopped by request");
            Mix_HaltMusic();
        } else if (elapsed >= TIMEOUT_MS) {
            warn("Audio playback timed out after {} seconds, stopping", TIMEOUT_MS / 1000);
            Mix_HaltMusic();
        } else {
            debug("Local audio playback completed successfully");
        }

        metrics->incrementSoundsPlayed();

    } catch (const std::exception &e) {
        const std::string errorMsg = fmt::format("Exception in audio playback thread: {}", e.what());
        error(errorMsg);
    } catch (...) {
        const std::string errorMsg = "Unknown exception in audio playback thread";
        error(errorMsg);
    }

    // Cleanup happens automatically via RAII guard
    gpioPins->playingSound(false);
    isPlaying_ = false;
    hasFinished_ = true;
}

} // namespace creatures
