#pragma once

#include "NativeAudioConfig.h"

namespace creatures::audio {

class AlsaMixerControl {
  public:
    static bool applyConfiguredVolume(const NativeAudioConfig &config);
};

} // namespace creatures::audio
