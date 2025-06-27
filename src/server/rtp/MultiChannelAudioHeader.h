
//
// MultiChannelAudioHeader.h
//

#pragma once

#include <cstdint>

namespace creatures :: rtp {

    // Enhanced packet structure for multi-channel audio
    struct MultiChannelAudioHeader {
        uint32_t timestamp;        // Frame timestamp
        uint32_t sampleCount;      // Samples per channel in this packet
        uint32_t sampleRate;       // Sample rate (48000)
        uint8_t channelCount;      // Number of channels (17)
        uint8_t reserved[3];       // Padding for alignment
    } __attribute__((packed));

} // namespace creatures :: rtp
