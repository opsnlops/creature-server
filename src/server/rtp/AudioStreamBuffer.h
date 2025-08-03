/**
 * @file AudioStreamBuffer.h
 * @brief Audio stream buffer with Opus encoding and caching support
 *
 * This file provides a buffer class for loading WAV files and encoding them
 * to Opus frames for RTP streaming, with intelligent caching support.
 */

#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "server/config.h"
#include "server/rtp/opus/OpusEncoderWrapper.h"
#include "util/AudioCache.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::rtp {

class AudioStreamBuffer {
  public:
    /// Factory method: load a 48 kHz / 17-channel WAV file and build Opus frames (with caching)
    static std::shared_ptr<AudioStreamBuffer> loadFromWavFile(const std::string &audioFilePath,
                                                              std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /// Set the audio cache instance to use for caching encoded files
    static void setAudioCacheInstance(std::shared_ptr<util::AudioCache> audioCacheInstance);

    /// Number of 10ms frames available (same for every channel)
    [[nodiscard]] std::size_t getFrameCount() const { return numberOfFramesPerChannel_; }

    /// Get encoded Opus payload for specified channel (0-16) at given frame index
    [[nodiscard]] const std::vector<uint8_t> &getEncodedFrame(uint8_t channelIndex, std::size_t frameIndex) const {
        return encodedOpusFrames_[channelIndex][frameIndex];
    }

  private:
    AudioStreamBuffer() = default;
    Result<size_t> loadWaveFile(const std::string &audioFilePath, std::shared_ptr<OperationSpan> parentSpan);

    /// Load from cache if available, otherwise encode and cache
    Result<size_t> loadWithCaching(const std::string &audioFilePath, std::shared_ptr<OperationSpan> parentSpan);

    /// Load cached data into this buffer
    void loadFromCachedAudioData(const util::AudioCache::CachedAudioData &cachedAudioData);

    std::size_t numberOfFramesPerChannel_{0};

    // Layout: encodedOpusFrames_[channel][frame] -> bytes
    std::array<std::vector<std::vector<uint8_t>>, RTP_STREAMING_CHANNELS> encodedOpusFrames_;

    // Static cache instance shared across all AudioStreamBuffer instances
    static std::shared_ptr<util::AudioCache> sharedAudioCacheInstance_;
};

} // namespace creatures::rtp