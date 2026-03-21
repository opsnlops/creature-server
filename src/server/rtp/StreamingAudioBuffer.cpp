#include "StreamingAudioBuffer.h"

#include <cstring>

#include "server/namespace-stuffs.h"

namespace creatures::rtp {

StreamingAudioBuffer::StreamingAudioBuffer(uint16_t audioChannel, uint32_t sampleRate,
                                            std::shared_ptr<OperationSpan> parentSpan)
    : audioChannel_(audioChannel), sampleRate_(sampleRate) {

    // Create Opus encoder: mono, 10ms frames at configured sample rate
    int frameSamples = static_cast<int>(sampleRate_ * RTP_FRAME_MS / 1000); // 480 samples for 48kHz/10ms
    encoder_ = std::make_unique<opus::Encoder>(static_cast<int>(sampleRate_), 1, frameSamples, RTP_BITRATE, true);

    // Pre-encode a silence frame for underrun handling
    std::vector<int16_t> silencePcm(frameSamples, 0);
    silenceFrame_ = encoder_->encode(silencePcm.data());

    // Reset encoder state after encoding silence (we want a clean start for real audio)
    encoder_->reset();

    debug("StreamingAudioBuffer created for channel {} at {} Hz ({} samples/frame)", audioChannel, sampleRate,
          frameSamples);

    (void)parentSpan; // Available for future tracing
}

void StreamingAudioBuffer::appendPcmData(const std::vector<uint8_t> &pcmData) {
    if (pcmData.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Convert raw bytes to int16_t samples and append to accumulator
    size_t numSamples = pcmData.size() / sizeof(int16_t);
    size_t prevSize = pcmAccumulator_.size();
    pcmAccumulator_.resize(prevSize + numSamples);
    std::memcpy(pcmAccumulator_.data() + prevSize, pcmData.data(), numSamples * sizeof(int16_t));

    // Encode all complete frames
    encodeAvailableFrames();
}

void StreamingAudioBuffer::markComplete() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Encode any remaining partial frame (pad with silence)
    int frameSamples = static_cast<int>(sampleRate_ * RTP_FRAME_MS / 1000);
    if (!pcmAccumulator_.empty()) {
        // Pad to frame boundary
        size_t remaining = pcmAccumulator_.size() % static_cast<size_t>(frameSamples);
        if (remaining > 0) {
            size_t padding = static_cast<size_t>(frameSamples) - remaining;
            pcmAccumulator_.resize(pcmAccumulator_.size() + padding, 0);
        }
        encodeAvailableFrames();
    }

    complete_.store(true);
    debug("StreamingAudioBuffer marked complete: {} encoded frames", encodedFrames_.size());
}

std::vector<uint8_t> StreamingAudioBuffer::getNextFrame() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (dispatchIndex_ < encodedFrames_.size()) {
        return encodedFrames_[dispatchIndex_++];
    }

    // Buffer underrun: return silence to keep RTP flowing
    return silenceFrame_;
}

bool StreamingAudioBuffer::isFinished() const {
    if (!complete_.load()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return dispatchIndex_ >= encodedFrames_.size();
}

size_t StreamingAudioBuffer::encodedFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return encodedFrames_.size();
}

void StreamingAudioBuffer::encodeAvailableFrames() {
    // Must be called with mutex_ held
    int frameSamples = static_cast<int>(sampleRate_ * RTP_FRAME_MS / 1000);

    while (pcmAccumulator_.size() >= static_cast<size_t>(frameSamples)) {
        // Encode one 10ms frame
        auto encoded = encoder_->encode(pcmAccumulator_.data());
        encodedFrames_.push_back(std::move(encoded));

        // Remove consumed samples from accumulator
        pcmAccumulator_.erase(pcmAccumulator_.begin(), pcmAccumulator_.begin() + frameSamples);
    }
}

} // namespace creatures::rtp
