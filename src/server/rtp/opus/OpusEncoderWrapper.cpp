// src/server/rtp/opus/OpusEncoderWrapper.cpp

#include <stdexcept>

#include <fmt/format.h>

#include "server/namespace-stuffs.h"

#include "OpusEncoderWrapper.h"

using namespace creatures::rtp::opus;

Encoder::Encoder(int sr, int ch, int fs, int br, bool fec)
    : frameSamples_(fs), scratch_(8000) // 8000 is enough for 20 ms mono @256k
{
    try {
        int err = 0;
        enc_ = opus_encoder_create(sr, ch, OPUS_APPLICATION_AUDIO, &err);
        if (err || !enc_) {
            const auto errorMessage = fmt::format("Error calling opus_encoder_create: {}", errorCodeToString(err));
            error(errorMessage);
            throw std::runtime_error(errorMessage);
        }

        /* --- CBR configuration for consistent packet timing --- */
        opus_encoder_ctl(enc_, OPUS_SET_BITRATE(br));          // Set target bitrate (96k/128k/256k)
        opus_encoder_ctl(enc_, OPUS_SET_VBR(0));               // Disable VBR - use CBR for consistent timing
        opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(10));       // Maximum quality psychoacoustic model
        opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

        if (fec) {
            opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));
            opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(10));
        }

        debug("Opus encoder configured for CBR mode at {} bps", br);

    } catch (const std::exception& e) {
        auto errorMessage = fmt::format("Error while creating opus encoder: {}", e.what());
        error(errorMessage);
        throw std::runtime_error(errorMessage);
    }
}

Encoder::~Encoder() { opus_encoder_destroy(enc_); }

std::vector<uint8_t> Encoder::encode(const int16_t* pcm)
{
    const int bytes = opus_encode(enc_,
                                  pcm,                // 20 ms mono samples
                                  frameSamples_,      // = 960 for 20ms at 48kHz
                                  scratch_.data(),
                                  static_cast<opus_int32>(scratch_.size()));
    if (bytes < 0) {
        const auto errorMessage = fmt::format("opus_encode failed: {}", errorCodeToString(bytes));
        error(errorMessage);
        throw std::runtime_error(errorMessage);
    }

    return {scratch_.data(), scratch_.data() + bytes};
}

void Encoder::reset()
{
    if (enc_) {
        opus_encoder_ctl(enc_, OPUS_RESET_STATE);
        debug("Opus encoder state reset to initial configuration");
    }
}