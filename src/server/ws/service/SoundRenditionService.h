#pragma once

#include <cstdint>
#include <filesystem>
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

    [[nodiscard]] creatures::Result<SoundRendition> renderWav(const std::filesystem::path &wavPath,
                                                              SoundRenditionFormat format) const;

    [[nodiscard]] creatures::Result<SoundRendition> renderMonoPcm(const std::vector<int16_t> &samples, int sampleRate,
                                                                  const creatures::voice::WavProvenance &provenance,
                                                                  SoundRenditionFormat format) const;

    [[nodiscard]] static Comments provenanceTags(const creatures::voice::WavProvenance &provenance);
};

} // namespace creatures::ws
