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
    const auto span = observability->createChildOperationSpan(
        "AudioStreamBuffer.loadWave", parentSpan);
    span->setAttribute("file_path", path);

    if (!std::filesystem::exists(path)) {
        span->setError("file not found");
        error("WAV file not found: {}", path);
        return false;
    }

    SDL_AudioSpec spec{};
    Uint8*   raw  = nullptr;
    Uint32   len  = 0;

    debug("Loading WAV file {}", path);
    if (!SDL_LoadWAV(path.c_str(), &spec, &raw, &len)) {
        const auto errorMessage = fmt::format("Failed to load WAV file: {}", path);
        span->setError(errorMessage);
        error(errorMessage);
        return false;
    }

    if (spec.freq != RTP_SRATE || spec.format != AUDIO_S16 ||
        spec.channels != RTP_STREAMING_CHANNELS)
    {
        const auto errorMessage = fmt::format("WAV file format not supported: {}, {} Hz, {} ch, format 0x{:X} (expected {} Hz, {} ch, format 0x{:X})",
            path, spec.freq, spec.channels, spec.format, RTP_SRATE, RTP_STREAMING_CHANNELS, AUDIO_S16);
        error(errorMessage);
        span->setError(errorMessage);
        SDL_FreeWAV(raw);
        return false;
    }

    const auto totalSamples = len / sizeof(int16_t);               // interleaved
    framesPerChannel_ = totalSamples /
                        (RTP_STREAMING_CHANNELS * RTP_SAMPLES);    // 10 ms frames
    if (!framesPerChannel_) {
        const auto errorMessage = fmt::format("WAV file too short: {} ({} samples)",
            path, totalSamples);
        error(errorMessage);
        span->setError(errorMessage);
        SDL_FreeWAV(raw);
        return false;
    }

    // Prepare encoder wrappers (one per channel)
    debug("encoding {} to opus...", path);
    try {
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
    }
    catch (const std::exception& e) {
        auto errorMessage = e.what();
        error("Error while encoding WAV to Opus: {}", errorMessage);
        span->setError(errorMessage);
        SDL_FreeWAV(raw);
        return false;
    }
    debug("encoding done");
    info("Loaded {} frames per channel from WAV file: {}", framesPerChannel_, path);

    SDL_FreeWAV(raw);
    span->setSuccess();
    return true;
}