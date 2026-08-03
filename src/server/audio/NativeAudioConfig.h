#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace creatures::audio {

inline constexpr size_t NATIVE_AUDIO_PERIOD_FRAMES = 256;
inline constexpr size_t NATIVE_AUDIO_RING_FRAMES = 8192;
inline constexpr size_t NATIVE_AUDIO_KEEPALIVE_FRAMES = 960; // 20 ms at 48 kHz
inline constexpr size_t NATIVE_AUDIO_QUEUE_TARGET_MS = 100;
inline constexpr uint16_t NATIVE_AUDIO_WAIT_MS = 5;

inline constexpr float NATIVE_DEFAULT_DIALOG_GAIN_DB = 0.0F;
inline constexpr float NATIVE_DEFAULT_BGM_GAIN_DB = 0.0F;
inline constexpr float NATIVE_DEFAULT_LIMITER_CEILING_DB = -1.0F;
inline constexpr float MIN_AUDIO_GAIN_DB = -90.0F;
inline constexpr float MAX_AUDIO_GAIN_DB = 12.0F;

inline constexpr const char *nativeAudioBackendName() {
#if defined(__APPLE__)
    return "coreaudio";
#elif defined(__linux__)
    return "alsa";
#else
    return "unsupported";
#endif
}

struct NativeAudioConfig {
    std::optional<std::string> deviceName;
    uint32_t sampleRate{48000};
    uint16_t channels{1};
    float dialogGainDb{NATIVE_DEFAULT_DIALOG_GAIN_DB};
    float bgmGainDb{NATIVE_DEFAULT_BGM_GAIN_DB};
    float limiterCeilingDb{NATIVE_DEFAULT_LIMITER_CEILING_DB};
    std::optional<uint8_t> outputVolumePercent;
    std::string alsaMixerCard{"default"};
    std::string alsaMixerElement;
};

} // namespace creatures::audio
