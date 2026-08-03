#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "AudioOutput.h"
#include "LocalAudioPlaybackCoordinator.h"
#include "NativeAudioConfig.h"

namespace creatures::audio {

enum class NativePlaybackMode { Main, Travel };

/**
 * Process-lifetime owner of the native output device.
 *
 * LocalAudioPlaybackCoordinator serializes clips; this service keeps the
 * opened device warm with a strictly bounded silence queue between clips.
 */
class NativeAudioPlaybackService {
  public:
    NativeAudioPlaybackService() = default;
    ~NativeAudioPlaybackService();
    NativeAudioPlaybackService(const NativeAudioPlaybackService &) = delete;
    NativeAudioPlaybackService &operator=(const NativeAudioPlaybackService &) = delete;

    bool initialize(NativeAudioConfig config);
    void shutdown();
    LocalAudioPlaybackCoordinator::PlaybackResult playFile(const std::string &filePath, NativePlaybackMode mode,
                                                           const std::atomic<bool> &stopRequested);

    [[nodiscard]] uint64_t underruns() const;

  private:
    bool refillKeepaliveLocked();
    bool restoreKeepaliveLocked();
    void keepaliveLoop();
    void finishPlayback();

    NativeAudioConfig config_;
    std::unique_ptr<AudioOutput> output_;
    mutable std::mutex outputMutex_;
    std::thread keepaliveThread_;
    std::atomic<bool> shutdownRequested_{false};
    std::atomic<bool> playbackActive_{false};
    bool initialized_{false};
};

} // namespace creatures::audio
