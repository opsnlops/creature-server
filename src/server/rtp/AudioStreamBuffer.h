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
    /// Should a load hold its buffer in the byte-budgeted retention set?
    enum class RetentionIntent {
        /// Audio that will play (again): worth keeping warm across playbacks.
        Retain,
        /// One-shot loads whose caller discards the buffer — cache prewarms,
        /// per-sentence streaming speech. Still memoized weakly (a concurrent
        /// load shares it), but never charged against the retention budget, so
        /// a long ad-hoc session can't evict warm show audio (issue #93).
        OneShot,
    };

    /// Factory method: load a 48 kHz / 17-channel WAV file and build Opus frames (with caching)
    static std::shared_ptr<AudioStreamBuffer> loadFromWavFile(const std::string &audioFilePath,
                                                              std::shared_ptr<OperationSpan> parentSpan = nullptr,
                                                              RetentionIntent retention = RetentionIntent::Retain);

    /// Set the audio cache instance to use for caching encoded files
    static void setAudioCacheInstance(std::shared_ptr<util::AudioCache> audioCacheInstance);

    /**
     * Set the in-memory retention budget. Injected explicitly at startup
     * (like setAudioCacheInstance) rather than latched from the environment on
     * first use — a load that happened before configuration used to freeze the
     * wrong machine's budget for the process lifetime (issue #93 review).
     * Until this is called, the smaller travel-server default applies.
     */
    static void setMemoRetainBytes(std::size_t bytes);
    [[nodiscard]] static std::size_t memoRetainBytes();
    [[nodiscard]] static std::size_t memoRetainedBytes();

    /// Drop any memoized buffer for this source path. Called by the storage
    /// facade whenever a sound file is written or invalidated, so a
    /// server-mediated rewrite can never be served from a stale buffer.
    static void invalidateMemo(const std::string &audioFilePath);

    /// Drop every memoized buffer (operator cache-invalidate, tests).
    static void clearMemo();

    /// Approximate memory footprint of the encoded payload (packet bytes plus
    /// per-frame vector overhead), computed once at load (issue #93).
    [[nodiscard]] std::size_t approximateBytes() const { return approximateBytes_; }

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

    /// Load cached data into this buffer (takes ownership — no payload copy)
    void loadFromCachedAudioData(util::AudioCache::CachedAudioData &&cachedAudioData);

    /// Sum packet bytes + per-frame overhead across all channels
    void computeApproximateBytes();

    std::size_t numberOfFramesPerChannel_{0};
    std::size_t approximateBytes_{0};

    // Layout: encodedOpusFrames_[channel][frame] -> bytes
    std::array<std::vector<std::vector<uint8_t>>, RTP_STREAMING_CHANNELS> encodedOpusFrames_;

    // Static cache instance shared across all AudioStreamBuffer instances
    static std::shared_ptr<util::AudioCache> sharedAudioCacheInstance_;
};

} // namespace creatures::rtp