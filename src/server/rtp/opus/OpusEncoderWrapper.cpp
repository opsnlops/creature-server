
// src/server/rtp/opus/OpusEncoderWrapper.cpp


#include <stdexcept>

#include "OpusEncoderWrapper.h"

using namespace creatures::rtp::opus;

Encoder::Encoder(int sr, int ch, int fs, int br, bool fec)
    : frameSamples_(fs), scratch_(4000)   // more than enough for 10 ms mono
{
    int err = 0;
    enc_ = opus_encoder_create(sr, ch, OPUS_APPLICATION_AUDIO, &err);
    if (err || !enc_)
        throw std::runtime_error("opus_encoder_create failed");

    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(br));
    opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(fec ? 1 : 0));
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(10));   // helps FEC
}

Encoder::~Encoder() { opus_encoder_destroy(enc_); }

std::vector<uint8_t> Encoder::encode(const int16_t* pcm)
{
    int bytes = opus_encode(enc_,
                            pcm,                // 10 ms mono samples
                            frameSamples_,      // = 480
                            scratch_.data(),
                            static_cast<opus_int32>(scratch_.size()));
    if (bytes < 0)
        throw std::runtime_error("opus_encode failed");

    return {scratch_.data(), scratch_.data() + bytes};
}