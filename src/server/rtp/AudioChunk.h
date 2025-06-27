//
// AudioChunk.h
//

#pragma once

#include <vector>

#include "server/namespace-stuffs.h"

namespace creatures :: rtp {

     /**
     * Represents a single chunk of multi-channel 16-bit PCM audio data
     * Optimized for direct RTP streaming without float conversion
     */
    struct AudioChunk {
        std::vector<int16_t> data;  // Interleaved 16-bit PCM data (ready for RTP)
        uint32_t sampleCount;       // Number of samples per channel in this chunk
        uint32_t channels;          // Number of channels
        uint32_t sampleRate;        // Sample rate (e.g., 48000 Hz)

        // Get data for a specific channel (0-based)
        std::vector<int16_t> getChannelData(uint8_t channel) const;

        // Get the raw data pointer for RTP transmission (no conversion needed!)
        const int16_t* getRawData() const { return data.data(); }

        // Get size in bytes for network transmission
        size_t getSizeInBytes() const { return data.size() * sizeof(int16_t); }
    };

} // namespace creatures :: rtp