/**
 * @file OpusEncoderWrapper.cpp
 * @brief Implementation of the Opus encoder wrapper class
 *
 * This file contains the implementation of the Opus audio encoder wrapper
 * with optimized settings for real-time RTP streaming.
 */

#include <stdexcept>

#include <fmt/format.h>

#include "server/namespace-stuffs.h"

#include "OpusEncoderWrapper.h"

using namespace creatures::rtp::opus;

Encoder::Encoder(int sampleRate, int channels, int samplesPerFrame, int bitrate, bool enableForwardErrorCorrection)
    : samplesPerFrame_(samplesPerFrame), encodingBuffer_(8000) // 8000 bytes is sufficient for 20ms mono at 256kbps
{
    try {
        int err = 0;
        opusEncoder_ = opus_encoder_create(sampleRate, channels, OPUS_APPLICATION_AUDIO, &err);
        if (err || !opusEncoder_) {
            const auto errorMessage = fmt::format("Error calling opus_encoder_create: {}", errorCodeToString(err));
            error(errorMessage);
            throw std::runtime_error(errorMessage);
        }

        // Configure Opus encoder for constant bitrate (CBR) mode to ensure consistent packet timing
        opus_encoder_ctl(opusEncoder_, OPUS_SET_BITRATE(bitrate)); // Set target bitrate (96k/128k/256k)
        opus_encoder_ctl(opusEncoder_, OPUS_SET_VBR(0));           // Disable variable bitrate for consistent timing
        opus_encoder_ctl(opusEncoder_, OPUS_SET_COMPLEXITY(10));   // Maximum quality psychoacoustic model
        opus_encoder_ctl(opusEncoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC)); // Optimize for music content

        if (enableForwardErrorCorrection) {
            opus_encoder_ctl(opusEncoder_, OPUS_SET_INBAND_FEC(1));
            opus_encoder_ctl(opusEncoder_, OPUS_SET_PACKET_LOSS_PERC(10));
        }

        debug("Opus encoder configured for constant bitrate mode at {} bps", bitrate);

    } catch (const std::exception &e) {
        auto errorMessage = fmt::format("Error while creating opus encoder: {}", e.what());
        error(errorMessage);
        throw std::runtime_error(errorMessage);
    }
}

Encoder::~Encoder() {
    if (opusEncoder_) {
        opus_encoder_destroy(opusEncoder_);
    }
}

std::vector<uint8_t> Encoder::encode(const int16_t *pcmData) {
    const int encodedBytes = opus_encode(opusEncoder_,
                                         pcmData,          // 20ms mono samples
                                         samplesPerFrame_, // 960 samples for 20ms at 48kHz
                                         encodingBuffer_.data(), static_cast<opus_int32>(encodingBuffer_.size()));
    if (encodedBytes < 0) {
        const auto errorMessage = fmt::format("opus_encode failed: {}", errorCodeToString(encodedBytes));
        error(errorMessage);
        throw std::runtime_error(errorMessage);
    }

    return {encodingBuffer_.data(), encodingBuffer_.data() + encodedBytes};
}

void Encoder::reset() {
    if (opusEncoder_) {
        opus_encoder_ctl(opusEncoder_, OPUS_RESET_STATE);
        debug("Opus encoder state reset to initial configuration");
    }
}