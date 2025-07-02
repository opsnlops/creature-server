//
// AudioStreamBuffer.cpp
//

#include <filesystem>


#include <spdlog/spdlog.h>
#include <SDL2/SDL.h>


#include "util/ObservabilityManager.h"
#include "AudioStreamBuffer.h"

namespace creatures {
    extern std::shared_ptr<ObservabilityManager> observability;
}


using namespace creatures;
using namespace creatures::rtp;

std::shared_ptr<AudioStreamBuffer>
AudioStreamBuffer::loadFromWav(const std::string& filePath,
                               std::shared_ptr<OperationSpan> parentSpan)
{
    auto buf = std::shared_ptr<AudioStreamBuffer>(new AudioStreamBuffer());
    return buf->loadWave(filePath, std::move(parentSpan)) ? buf : nullptr;
}

bool AudioStreamBuffer::loadWave(const std::string& path,
                                 std::shared_ptr<OperationSpan> parentSpan)
{
    auto span = observability->createChildOperationSpan(
        "AudioStreamBuffer.loadWave", parentSpan);
    span->setAttribute("file_path", path);

    if (!std::filesystem::exists(path)) {
        span->setError("file not found");
        spdlog::error("WAV file not found: {}", path);
        return false;
    }

    SDL_AudioSpec spec{};
    Uint8*   raw  = nullptr;
    Uint32   len  = 0;

    if (!SDL_LoadWAV(path.c_str(), &spec, &raw, &len)) {
        span->setError("SDL_LoadWAV failed");
        spdlog::error("SDL_LoadWAV failed: {}", SDL_GetError());
        return false;
    }

    if (spec.freq != RTP_SRATE || spec.format != AUDIO_S16 ||
        spec.channels != RTP_STREAMING_CHANNELS)
    {
        span->setError("bad WAV format");
        spdlog::error("Bad WAV: {} Hz / {} ch / fmt 0x{:X} (need 48000/17/16-bit)",
                      spec.freq, spec.channels, spec.format);
        SDL_FreeWAV(raw);
        return false;
    }

    const auto totalSamples = len / sizeof(int16_t);               // interleaved
    framesPerChannel_ = totalSamples /
                        (RTP_STREAMING_CHANNELS * RTP_SAMPLES);    // 10 ms frames
    if (!framesPerChannel_) {
        span->setError("audio too short");
        SDL_FreeWAV(raw);
        return false;
    }

    // Prepare encoder wrappers (one per channel)
    std::array<opus::Encoder, RTP_STREAMING_CHANNELS> encoders;
    for (auto& vec : encodedFrames_)
        vec.resize(framesPerChannel_);

    const int16_t* pcm = reinterpret_cast<int16_t*>(raw);

    for (std::size_t f = 0; f < framesPerChannel_; ++f) {
        const int16_t* frameBase =
            pcm + f * RTP_SAMPLES * RTP_STREAMING_CHANNELS;

        for (uint8_t ch = 0; ch < RTP_STREAMING_CHANNELS; ++ch) {
            std::array<int16_t, RTP_SAMPLES> mono{};
            for (std::size_t s = 0; s < RTP_SAMPLES; ++s)
                mono[s] = frameBase[s * RTP_STREAMING_CHANNELS + ch];

            encodedFrames_[ch][f] = encoders[ch].encode(mono.data());
        }
    }

    SDL_FreeWAV(raw);
    span->setSuccess();
    return true;
}