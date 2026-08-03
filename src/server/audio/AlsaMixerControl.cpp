#include "AlsaMixerControl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include <spdlog/spdlog.h>

#if defined(CREATURE_HAS_ALSA)
#include <alsa/asoundlib.h>
#endif

namespace creatures::audio {

#if defined(CREATURE_HAS_ALSA)
namespace {

class MixerHandle {
  public:
    ~MixerHandle() {
        if (handle_ != nullptr) {
            snd_mixer_close(handle_);
        }
    }
    snd_mixer_t **address() { return &handle_; }
    snd_mixer_t *get() const { return handle_; }

  private:
    snd_mixer_t *handle_{nullptr};
};

snd_mixer_elem_t *findPlaybackElement(snd_mixer_t *mixer, const std::string &configuredName) {
    snd_mixer_elem_t *fallback = nullptr;
    constexpr std::array<const char *, 3> preferredNames{"Master", "PCM", "Speaker"};
    std::array<snd_mixer_elem_t *, preferredNames.size()> preferredElements{};

    for (snd_mixer_elem_t *element = snd_mixer_first_elem(mixer); element != nullptr;
         element = snd_mixer_elem_next(element)) {
        if (!snd_mixer_selem_is_active(element) || !snd_mixer_selem_has_playback_volume(element)) {
            continue;
        }
        const char *name = snd_mixer_selem_get_name(element);
        if (name == nullptr) {
            continue;
        }
        if (!configuredName.empty() && configuredName == name) {
            return element;
        }
        if (configuredName.empty()) {
            for (size_t index = 0; index < preferredNames.size(); ++index) {
                if (std::string{name} == preferredNames[index]) {
                    preferredElements[index] = element;
                }
            }
            if (fallback == nullptr) {
                fallback = element;
            }
        }
    }
    for (snd_mixer_elem_t *element : preferredElements) {
        if (element != nullptr) {
            return element;
        }
    }
    return fallback;
}

} // namespace
#endif

bool AlsaMixerControl::applyConfiguredVolume(const NativeAudioConfig &config) {
    if (!config.outputVolumePercent.has_value()) {
        return true;
    }

#if defined(CREATURE_HAS_ALSA)
    MixerHandle mixer;
    int result = snd_mixer_open(mixer.address(), 0);
    if (result < 0) {
        spdlog::warn("Unable to open ALSA mixer: {}", snd_strerror(result));
        return false;
    }
    if ((result = snd_mixer_attach(mixer.get(), config.alsaMixerCard.c_str())) < 0 ||
        (result = snd_mixer_selem_register(mixer.get(), nullptr, nullptr)) < 0 ||
        (result = snd_mixer_load(mixer.get())) < 0) {
        spdlog::warn("Unable to initialize ALSA mixer card '{}': {}", config.alsaMixerCard, snd_strerror(result));
        return false;
    }

    snd_mixer_elem_t *element = findPlaybackElement(mixer.get(), config.alsaMixerElement);
    if (element == nullptr) {
        spdlog::warn("ALSA mixer card '{}' has no usable playback-volume element{}", config.alsaMixerCard,
                     config.alsaMixerElement.empty() ? "" : " named '" + config.alsaMixerElement + "'");
        return false;
    }

    long minimum = 0;
    long maximum = 0;
    if (snd_mixer_selem_get_playback_volume_range(element, &minimum, &maximum) < 0 || maximum <= minimum) {
        spdlog::warn("Unable to read ALSA volume range for '{}'", snd_mixer_selem_get_name(element));
        return false;
    }

    const auto percent = std::clamp<int>(*config.outputVolumePercent, 0, 100);
    const long value =
        minimum + static_cast<long>(std::lround((maximum - minimum) * (static_cast<double>(percent) / 100.0)));
    result = snd_mixer_selem_set_playback_volume_all(element, value);
    if (result < 0) {
        spdlog::warn("Unable to set ALSA playback volume on '{}': {}", snd_mixer_selem_get_name(element),
                     snd_strerror(result));
        return false;
    }
    spdlog::info("Set ALSA playback volume to {}% on card '{}', element '{}'", percent, config.alsaMixerCard,
                 snd_mixer_selem_get_name(element));
    return true;
#else
    spdlog::warn("Output volume is configured, but this build does not include ALSA mixer support");
    return false;
#endif
}

} // namespace creatures::audio
