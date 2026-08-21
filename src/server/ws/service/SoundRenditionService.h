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

    /// What an animation-document lookup can contribute when the WAV's own iXML
    /// is missing pieces: a title (#148) and the actual cast (#153) — an iXML
    /// TRACK_LIST is a channel map, so lane names may include creatures that
    /// don't perform in this piece; the animation's tracks are the truth.
    struct FallbackMetadata {
        std::string title;  // used when the iXML has no title
        std::string artist; // used when the iXML has no script (pre-joined, e.g. "Beaky, Mango")
    };

    /// Lazy source for FallbackMetadata. Only invoked when the iXML is missing a
    /// title or a script, so callers can back it with a database lookup without
    /// paying for it on fully-tagged files.
    using MetadataProvider = std::function<FallbackMetadata()>;

    [[nodiscard]] creatures::Result<SoundRendition> renderWav(const std::filesystem::path &wavPath,
                                                              SoundRenditionFormat format,
                                                              const MetadataProvider &fallback = {}) const;

    [[nodiscard]] creatures::Result<SoundRendition> renderMonoPcm(const std::vector<int16_t> &samples, int sampleRate,
                                                                  const creatures::voice::WavProvenance &provenance,
                                                                  SoundRenditionFormat format,
                                                                  const std::string &artistOverride = {}) const;

    /// `artistOverride` replaces the lane-derived ARTIST when the provenance has
    /// no script; script speakers always win when present (#153).
    [[nodiscard]] static Comments provenanceTags(const creatures::voice::WavProvenance &provenance,
                                                 const std::string &artistOverride = {});
};

} // namespace creatures::ws
