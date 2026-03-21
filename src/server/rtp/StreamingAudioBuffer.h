#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "server/config.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/opus/OpusEncoderWrapper.h" // creatures::rtp::opus::Encoder
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::rtp {

/**
 * StreamingAudioBuffer
 *
 * Accepts PCM audio chunks incrementally from the ElevenLabs WebSocket stream,
 * Opus-encodes them in 10ms increments, and makes encoded frames available for
 * RTP dispatch as they arrive.
 *
 * Audio data is placed on a specific channel of the 17-channel layout.
 * When frames aren't yet available, silence frames are provided to keep
 * the RTP stream flowing smoothly.
 *
 * Thread safety:
 * - appendPcmData() is called from the WebSocket receive thread
 * - getNextFrame() is called from the event loop's RTP dispatch
 * - A mutex protects the PCM buffer and encoded frames
 */
class StreamingAudioBuffer {
  public:
    /**
     * Create a streaming audio buffer for a specific channel.
     *
     * @param audioChannel Target audio channel (1-based, 1-16 for creatures, 17 for BGM)
     * @param sampleRate Sample rate in Hz (typically 48000)
     * @param parentSpan Optional observability span
     */
    StreamingAudioBuffer(uint16_t audioChannel, uint32_t sampleRate = 48000,
                          std::shared_ptr<OperationSpan> parentSpan = nullptr);

    ~StreamingAudioBuffer() = default;

    // Non-copyable
    StreamingAudioBuffer(const StreamingAudioBuffer &) = delete;
    StreamingAudioBuffer &operator=(const StreamingAudioBuffer &) = delete;

    /**
     * Append raw PCM audio data (mono, 16-bit, at configured sample rate).
     *
     * The data will be Opus-encoded in 10ms increments as enough samples
     * accumulate. Thread-safe.
     *
     * @param pcmData Raw PCM audio bytes (little-endian 16-bit samples)
     */
    void appendPcmData(const std::vector<uint8_t> &pcmData);

    /**
     * Signal that no more audio data will arrive.
     *
     * Flushes any remaining partial frame as a final Opus packet.
     */
    void markComplete();

    /**
     * Get the next encoded Opus frame for this channel.
     *
     * If no frame is available yet, returns a silence frame to keep
     * the RTP stream flowing.
     *
     * @return Opus-encoded frame data
     */
    std::vector<uint8_t> getNextFrame();

    /**
     * Check if all audio has been encoded and dispatched.
     */
    [[nodiscard]] bool isFinished() const;

    /**
     * Get the total number of encoded frames available.
     */
    [[nodiscard]] size_t encodedFrameCount() const;

    /**
     * Get the audio channel this buffer writes to.
     */
    [[nodiscard]] uint16_t getAudioChannel() const { return audioChannel_; }

  private:
    uint16_t audioChannel_;
    uint32_t sampleRate_;

    // Mutex protecting all mutable state
    mutable std::mutex mutex_;

    // Incoming PCM accumulation buffer
    std::vector<int16_t> pcmAccumulator_;

    // Encoded Opus frames (each is one 10ms packet)
    std::vector<std::vector<uint8_t>> encodedFrames_;

    // Current dispatch position
    size_t dispatchIndex_ = 0;

    // Completion flag
    std::atomic<bool> complete_{false};

    // Opus encoder
    std::unique_ptr<opus::Encoder> encoder_;

    // Pre-encoded silence frame for underrun
    std::vector<uint8_t> silenceFrame_;

    /**
     * Encode all complete 10ms frames from the PCM accumulator.
     * Must be called with mutex_ held.
     */
    void encodeAvailableFrames();
};

} // namespace creatures::rtp
