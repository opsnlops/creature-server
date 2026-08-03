#include "DecodedAudioStream.h"

#include <limits>
#include <utility>

#include <fmt/format.h>
#include <miniaudio.h>

#include "server/namespace-stuffs.h"

namespace creatures::audio {

struct DecodedAudioStream::Impl {
    ma_decoder decoder{};
    uint64_t frames{0};
    uint32_t rate{0};
    uint16_t channelCount{0};
    bool initialized{false};

    ~Impl() {
        if (initialized) {
            ma_decoder_uninit(&decoder);
        }
    }
};

DecodedAudioStream::DecodedAudioStream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DecodedAudioStream::~DecodedAudioStream() = default;

Result<std::shared_ptr<DecodedAudioStream>>
DecodedAudioStream::open(const std::string &filePath, uint32_t outputSampleRate, uint16_t outputChannels) {
    if (filePath.empty() || outputSampleRate == 0 || outputChannels == 0) {
        return Result<std::shared_ptr<DecodedAudioStream>>{
            ServerError(ServerError::InvalidData, "Invalid decoded-audio stream configuration")};
    }
    auto impl = std::make_unique<Impl>();
    const ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_s16, outputChannels, outputSampleRate);
    const ma_result initResult = ma_decoder_init_file(filePath.c_str(), &decoderConfig, &impl->decoder);
    if (initResult != MA_SUCCESS) {
        return Result<std::shared_ptr<DecodedAudioStream>>{
            ServerError(ServerError::InvalidData, fmt::format("Unable to decode audio file '{}': {}", filePath,
                                                              ma_result_description(initResult)))};
    }
    impl->initialized = true;
    impl->rate = outputSampleRate;
    impl->channelCount = outputChannels;
    // Do not ask miniaudio for the decoded length here: for MP3 it obtains the
    // answer by decoding the entire file, which would duplicate the playback
    // pass and make cooperative replacement wait on an uncancellable scan.
    // NativeAudioPlaybackService enforces its duration bound while streaming.
    return Result<std::shared_ptr<DecodedAudioStream>>{
        std::shared_ptr<DecodedAudioStream>(new DecodedAudioStream(std::move(impl)))};
}

Result<size_t> DecodedAudioStream::readFrames(std::span<int16_t> output) {
    if (!impl_ || impl_->channelCount == 0 || output.size() % impl_->channelCount != 0) {
        return Result<size_t>{ServerError(ServerError::InvalidData, "Decoded audio buffer is not frame aligned")};
    }
    const size_t requestedFrames = output.size() / impl_->channelCount;
    if (requestedFrames > std::numeric_limits<ma_uint64>::max()) {
        return Result<size_t>{ServerError(ServerError::InvalidData, "Decoded audio request is too large")};
    }
    ma_uint64 framesRead = 0;
    const ma_result result = ma_decoder_read_pcm_frames(&impl_->decoder, output.data(),
                                                        static_cast<ma_uint64>(requestedFrames), &framesRead);
    if (result != MA_SUCCESS && result != MA_AT_END) {
        return Result<size_t>{ServerError(ServerError::InvalidData,
                                          fmt::format("Audio decode failed: {}", ma_result_description(result)))};
    }
    return Result<size_t>{static_cast<size_t>(framesRead)};
}

uint64_t DecodedAudioStream::totalFrames() const { return impl_ ? impl_->frames : 0; }
uint32_t DecodedAudioStream::sampleRate() const { return impl_ ? impl_->rate : 0; }
uint16_t DecodedAudioStream::channels() const { return impl_ ? impl_->channelCount : 0; }

} // namespace creatures::audio
