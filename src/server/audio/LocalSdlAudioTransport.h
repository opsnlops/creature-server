#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "AudioTransport.h"

namespace creatures {

/**
 * LocalSdlAudioTransport - SDL local playback audio transport implementation
 *
 * Plays audio through SDL_mixer on the local audio device. Runs in a background
 * thread that is spawned on start() and runs independently until the audio completes
 * or stop() is called.
 *
 * This is a "fire and forget" transport - once started, it doesn't need per-frame
 * dispatch from the PlaybackRunnerEvent. The runner only needs to call stop() on
 * cancellation.
 */
class LocalSdlAudioTransport : public AudioTransport {
  public:
    LocalSdlAudioTransport();
    ~LocalSdlAudioTransport() override;

    Result<void> start(std::shared_ptr<PlaybackSession> session) override;

    void stop() override;

    [[nodiscard]] bool needsPerFrameDispatch() const override { return false; }

    Result<framenum_t> dispatchNextChunk(framenum_t currentFrame) override {
        // SDL runs independently, no per-frame dispatch needed
        return Result<framenum_t>{currentFrame};
    }

    [[nodiscard]] bool isFinished() const override;

  private:
    std::shared_ptr<PlaybackSession> session_;
    std::thread audioThread_;
    std::atomic<bool> shouldStop_{false};
    std::atomic<bool> isPlaying_{false};
    std::atomic<bool> hasFinished_{false};

    /**
     * Audio playback thread function
     *
     * Runs SDL_mixer playback in background, extracted from MusicEvent::playLocalAudio()
     */
    void audioThreadFunc(std::string filePath);
};

} // namespace creatures
