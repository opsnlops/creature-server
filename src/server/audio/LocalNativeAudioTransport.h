#pragma once

#include <memory>
#include <string>

#include "AudioTransport.h"
#include "LocalAudioPlaybackCoordinator.h"
#include "NativeAudioPlaybackService.h"

namespace creatures {

class LocalNativeAudioTransport : public AudioTransport {
  public:
    explicit LocalNativeAudioTransport(audio::NativePlaybackMode mode);
    ~LocalNativeAudioTransport() override;

    Result<void> start(std::shared_ptr<PlaybackSession> session) override;
    void stop() override;
    [[nodiscard]] bool isFinished() const override;
    [[nodiscard]] bool needsPerFrameDispatch() const override { return false; }
    Result<framenum_t> dispatchNextChunk(framenum_t currentFrame) override { return Result<framenum_t>{currentFrame}; }

    static audio::LocalAudioPlaybackCoordinator::PlaybackResult
    playFileBlocking(const std::string &filePath, audio::NativePlaybackMode mode,
                     const std::atomic<bool> &stopRequested);

  private:
    audio::NativePlaybackMode mode_;
    std::shared_ptr<audio::LocalAudioPlaybackCoordinator::Handle> playbackHandle_;
};

} // namespace creatures
