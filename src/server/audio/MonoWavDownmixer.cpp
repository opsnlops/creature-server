//
// MonoWavDownmixer.cpp
// Renders multi-channel WAV files down to mono for travel mode
//

#include "MonoWavDownmixer.h"

#include <algorithm>
#include <filesystem>

#include <SDL.h>
#include <fmt/format.h>

#include "server/namespace-stuffs.h"

namespace creatures::audio {

void downmixToMono(const int16_t *interleaved, const size_t frames, const int channels, int16_t *out) {
    for (size_t f = 0; f < frames; ++f) {
        const int16_t *frameBase = interleaved + f * static_cast<size_t>(channels);
        int32_t acc = 0;
        for (int ch = 0; ch < channels; ++ch) {
            acc += frameBase[ch];
        }
        out[f] =
            static_cast<int16_t>(std::clamp(acc, static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
    }
}

Result<MonoWav> loadWavAsMono(const std::string &filePath) {

    if (!std::filesystem::exists(filePath) || !std::filesystem::is_regular_file(filePath)) {
        const auto errorMsg = fmt::format("WAV file not found: {}", filePath);
        error(errorMsg);
        return Result<MonoWav>{ServerError(ServerError::NotFound, errorMsg)};
    }

    SDL_AudioSpec spec{};
    Uint8 *raw = nullptr;
    Uint32 len = 0;

    if (!SDL_LoadWAV(filePath.c_str(), &spec, &raw, &len)) {
        const auto errorMsg = fmt::format("SDL failed to load WAV file '{}': {}", filePath, SDL_GetError());
        error(errorMsg);
        return Result<MonoWav>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // RAII wrapper to ensure SDL memory gets freed
    struct SDLWavGuard {
        Uint8 *data;
        ~SDLWavGuard() {
            if (data)
                SDL_FreeWAV(data);
        }
    } guard{raw};

    if (spec.format != AUDIO_S16) {
        const auto errorMsg = fmt::format("WAV file '{}' is not signed 16-bit PCM (format 0x{:X})", filePath,
                                          static_cast<unsigned>(spec.format));
        error(errorMsg);
        return Result<MonoWav>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    if (spec.channels < 1) {
        const auto errorMsg = fmt::format("WAV file '{}' has no channels", filePath);
        error(errorMsg);
        return Result<MonoWav>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    const auto totalSamples = static_cast<size_t>(len) / sizeof(int16_t);
    const auto frames = totalSamples / spec.channels;
    if (frames == 0) {
        const auto errorMsg = fmt::format("WAV file '{}' contains no audio", filePath);
        error(errorMsg);
        return Result<MonoWav>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    MonoWav mono;
    mono.sampleRate = spec.freq;
    mono.samples.resize(frames);
    downmixToMono(reinterpret_cast<const int16_t *>(raw), frames, spec.channels, mono.samples.data());

    debug("downmixed '{}' to mono: {} channels, {} frames, {} Hz", filePath, spec.channels, frames, spec.freq);

    return Result<MonoWav>{mono};
}

} // namespace creatures::audio
