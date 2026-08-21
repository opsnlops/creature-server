#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "server/voice/IxmlWriter.h"
#include "util/Result.h"

namespace creatures::ws {

enum class SoundRenditionFormat { Mp3, OggOpus };

struct SoundRendition {
    std::vector<uint8_t> bytes;
    std::string mimeType;
    std::string extension;
};

class SoundRenditionService {
  public:
    using Comments = std::vector<std::pair<std::string, std::string>>;

    /// Lazy source for a title when the WAV's iXML has none (older renders and
    /// deliberately-lean ad-hoc files). Only invoked in that case, so callers can
    /// back it with a database lookup without paying for it on tagged files (#148).
    using TitleProvider = std::function<std::string()>;

    [[nodiscard]] creatures::Result<SoundRendition> renderWav(const std::filesystem::path &wavPath,
                                                              SoundRenditionFormat format,
                                                              const TitleProvider &fallbackTitle = {}) const;

    [[nodiscard]] creatures::Result<SoundRendition> renderMonoPcm(const std::vector<int16_t> &samples, int sampleRate,
                                                                  const creatures::voice::WavProvenance &provenance,
                                                                  SoundRenditionFormat format) const;

    [[nodiscard]] static Comments provenanceTags(const creatures::voice::WavProvenance &provenance);
};

} // namespace creatures::ws
