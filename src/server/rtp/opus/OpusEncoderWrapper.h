// src/server/rtp/opus/OpusEncoderWrapper.h

#pragma once

#include <opus/opus.h>
#include <vector>

#include "server/config.h"

namespace creatures::rtp::opus {

    class Encoder {
    public:
        Encoder(int sampleRate      = 48000,
                int channels        = 1,
                int frameSamples    = RTP_SAMPLES,
                int bitrate         = 64000,        // tweak later
                bool enableFec      = true);

        ~Encoder();

        // Encodes one PCM frame (int16) → returns encoded bytes
        std::vector<uint8_t> encode(const int16_t* pcm);

    private:
        OpusEncoder* enc_{nullptr};
        const int    frameSamples_;
        std::vector<uint8_t> scratch_;
    };

} // namespace creatures::rtp::opus