// src/server/rtp/opus/OpusEncoderWrapper.cpp

#include <stdexcept>

#include <fmt/format.h>

#include "server/namespace-stuffs.h"

#include "OpusEncoderWrapper.h"

using namespace creatures::rtp::opus;

Encoder::Encoder(int sr, int ch, int fs, int br, bool fec)
    : frameSamples_(fs), scratch_(4000) // 4000 is enough for 10 ms mono @128k
{
    try {
        int err = 0;
        enc_ = opus_encoder_create(sr, ch, OPUS_APPLICATION_AUDIO, &err);
        if (err || !enc_) {
            const auto errorMessage = fmt::format("Error calling opus_encoder_create: {}", errorCodeToString(err));
            error(errorMessage);
            throw std::runtime_error(errorMessage);
        }

        /* --- quality knobs --- */
        opus_encoder_ctl(enc_, OPUS_SET_BITRATE(br));          // 96k/128k caller picks
        opus_encoder_ctl(enc_, OPUS_SET_VBR(1));               // enable VBR
        opus_encoder_ctl(enc_, OPUS_SET_VBR_CONSTRAINT(0));    // unconstrained
        opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(10));       // best psycho model
        opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        if (fec) {
            opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));
            opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(10));
        }
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
                                  pcm,                // 10 ms mono samples
                                  frameSamples_,      // = 480
                                  scratch_.data(),
                                  static_cast<opus_int32>(scratch_.size()));
    if (bytes < 0)
        throw std::runtime_error("opus_encode failed");

    return {scratch_.data(), scratch_.data() + bytes};
}

void Encoder::reset()
{
    if (enc_) {
        opus_encoder_ctl(enc_, OPUS_RESET_STATE);
        debug("Opus encoder state reset");
    }
}