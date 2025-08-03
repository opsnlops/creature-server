/**
 * @file OpusEncoderWrapper.h
 * @brief Wrapper class for Opus audio encoding functionality
 *
 * This file provides a C++ wrapper around the libopus encoder with
 * optimized settings for real-time audio streaming over RTP.
 */

#pragma once

#include <opus/opus.h>
#include <vector>

#include "server/config.h"

namespace creatures::rtp::opus {

class Encoder {
  public:
    Encoder(int sampleRate = RTP_SRATE, int channels = 1, int frameSamples = RTP_SAMPLES, int bitrate = RTP_BITRATE,
            bool enableForwardErrorCorrection = true);

    ~Encoder();

    // Encodes one PCM frame (int16) and returns encoded bytes
    std::vector<uint8_t> encode(const int16_t *pcmData);

    // Reset encoder state
    void reset();

    // Get handle for direct opus_encoder_ctl calls if needed
    OpusEncoder *getEncoderHandle() { return opusEncoder_; }

  private:
    OpusEncoder *opusEncoder_{nullptr};
    const int samplesPerFrame_;
    std::vector<uint8_t> encodingBuffer_;

    static std::string errorCodeToString(const int errorCode) {
        switch (errorCode) {
        case OPUS_BAD_ARG:
            return "OPUS_BAD_ARG";
        case OPUS_UNIMPLEMENTED:
            return "OPUS_UNIMPLEMENTED";
        case OPUS_INTERNAL_ERROR:
            return "OPUS_INTERNAL_ERROR";
        case OPUS_INVALID_STATE:
            return "OPUS_INVALID_STATE";
        case OPUS_ALLOC_FAIL:
            return "OPUS_ALLOC_FAIL";
        default:
            return "Unknown error code";
        }
    }
};

} // namespace creatures::rtp::opus