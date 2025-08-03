/**
 * @file AudioChunk.h
 * @brief Audio chunk structure for multi-channel PCM data
 *
 * This file provides a structure for representing chunks of multi-channel
 * 16-bit PCM audio data optimized for direct RTP streaming.
 */

#pragma once

#include <vector>

#include "server/namespace-stuffs.h"

namespace creatures ::rtp {

/**
 * Represents a single chunk of multi-channel 16-bit PCM audio data
 * Optimized for direct RTP streaming without float conversion
 */
struct AudioChunk {
    std::vector<int16_t> interleavedPcmData; // Interleaved 16-bit PCM data (ready for RTP)
    uint32_t samplesPerChannel;              // Number of samples per channel in this chunk
    uint32_t numberOfChannels;               // Number of channels
    uint32_t sampleRateInHz;                 // Sample rate (e.g., 48000 Hz)

    // Get data for a specific channel (0-based)
    [[nodiscard]] std::vector<int16_t> getChannelData(uint8_t channelIndex) const;

    // Get the raw data pointer for RTP transmission (no conversion needed!)
    [[nodiscard]] const int16_t *getRawDataPointer() const { return interleavedPcmData.data(); }

    // Get size in bytes for network transmission
    [[nodiscard]] size_t getSizeInBytes() const { return interleavedPcmData.size() * sizeof(int16_t); }
};

} // namespace creatures::rtp