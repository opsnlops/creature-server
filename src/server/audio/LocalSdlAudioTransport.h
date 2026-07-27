#pragma once

#include <memory>
#include <string>

#include "AudioTransport.h"
#include "LocalAudioPlaybackCoordinator.h"

namespace creatures {

/**
 * LocalSdlAudioTransport - SDL local playback audio transport implementation
 *
 * Plays audio through SDL_mixer on the local audio device. Blocking SDL work is
 * submitted to the process-wide LocalAudioPlaybackCoordinator.
 *
 * This is a "fire and forget" transport - once started, it doesn't need
 * per-frame dispatch from the PlaybackRunnerEvent. The runner only needs to
 * cancel its lightweight coordinator handle.
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

    static audio::LocalAudioPlaybackCoordinator::PlaybackResult
    playFileBlocking(const std::string &filePath, const std::atomic<bool> &stopRequested);

  private:
    std::shared_ptr<audio::LocalAudioPlaybackCoordinator::Handle> playbackHandle_;
};

} // namespace creatures
