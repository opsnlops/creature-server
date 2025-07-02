//
// AudioStreamBuffer.h
//
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "server/config.h"
#include "server/rtp/opus/OpusEncoderWrapper.h"
#include "util/ObservabilityManager.h"

namespace creatures::rtp {

    class AudioStreamBuffer {
    public:
        /// Factory: load a 48 kHz / 17-channel WAV and build Opus frames
        static std::shared_ptr<AudioStreamBuffer>
        loadFromWav(const std::string& filePath,
                    std::shared_ptr<OperationSpan> parentSpan = nullptr);

        /// Number of 10 ms frames available (same for every channel)
        [[nodiscard]] std::size_t frameCount() const { return framesPerChannel_; }

        /// Encoded Opus payload for `channel` (0-16) at `frame` index
        [[nodiscard]] const std::vector<uint8_t>&
        frame(uint8_t channel, std::size_t frame) const {
            return encodedFrames_[channel][frame];
        }

    private:
        AudioStreamBuffer() = default;
        bool loadWave(const std::string& filePath,
                      std::shared_ptr<OperationSpan> parentSpan);

        std::size_t framesPerChannel_{0};

        // Layout: encodedFrames_[channel][frame] -> bytes
        std::array<std::vector<std::vector<uint8_t>>, RTP_STREAMING_CHANNELS>
            encodedFrames_;
    };

} // namespace creatures::rtp