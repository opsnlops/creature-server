// src/server/rtp/opus/OpusEncoderWrapper.h

#pragma once

#include <opus/opus.h>
#include <vector>

#include "server/config.h"

namespace creatures::rtp::opus {

    class Encoder {
    public:
        Encoder(int sampleRate  = RTP_SRATE,
        int channels            = 1,
        int frameSamples        = RTP_SAMPLES,
        int bitrate             = RTP_BITRATE,
        bool enableFec          = true);

        ~Encoder();

        // Encodes one PCM frame (int16) → returns encoded bytes
        std::vector<uint8_t> encode(const int16_t* pcm);

    private:
        OpusEncoder* enc_{nullptr};
        const int    frameSamples_;
        std::vector<uint8_t> scratch_;

        static std::string errorCodeToString(const int err) {
            switch (err) {
                case OPUS_BAD_ARG: return "OPUS_BAD_ARG";
                case OPUS_UNIMPLEMENTED: return "OPUS_UNIMPLEMENTED";
                case OPUS_INTERNAL_ERROR: return "OPUS_INTERNAL_ERROR";
                case OPUS_INVALID_STATE: return "OPUS_INVALID_STATE";
                case OPUS_ALLOC_FAIL: return "OPUS_ALLOC_FAIL";
                default: return "Unknown error code";
            }
        }
    };

} // namespace creatures::rtp::opus