#pragma once

#include <memory>
#include <string>

#include "AudioTransport.h"
#include "LocalAudioPlaybackCoordinator.h"

namespace creatures {

/**
 * TravelMonoAudioTransport - mono local playback for travel mode
 *
 * Travel mode runs the server and the controllers on one host with a pair of
 * speakers plugged into the server, so the 17-channel animation tracks get
 * downmixed to mono and played on the local sound device via plain SDL
 * (SDL_QueueAudio). Like LocalSdlAudioTransport this is a fire-and-forget
 * transport backed by the single process-wide local-audio worker.
 */
class TravelMonoAudioTransport : public AudioTransport {
  public:
    TravelMonoAudioTransport();
    ~TravelMonoAudioTransport() override;

    Result<void> start(std::shared_ptr<PlaybackSession> session) override;

    void stop() override;

    [[nodiscard]] bool needsPerFrameDispatch() const override { return false; }

    Result<framenum_t> dispatchNextChunk(framenum_t currentFrame) override {
        // SDL runs independently, no per-frame dispatch needed
        return Result<framenum_t>{currentFrame};
    }

    [[nodiscard]] bool isFinished() const override;

    /**
     * Downmix a WAV file to mono and play it on the local device, blocking
     * until playback finishes or `shouldStop` becomes true.
     *
     * Shared with MusicEvent so the console's ad-hoc "play sound" endpoint
     * gets the same downmix treatment in travel mode.
     *
     * @param filePath path to the WAV file
     * @param stopRequested cooperative early-stop flag
     */
    static audio::LocalAudioPlaybackCoordinator::PlaybackResult
    playFileBlocking(const std::string &filePath, const std::atomic<bool> &stopRequested);

  private:
    std::shared_ptr<audio::LocalAudioPlaybackCoordinator::Handle> playbackHandle_;
};

} // namespace creatures
