#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "server/voice/IxmlWriter.h"
#include "util/Result.h"

namespace creatures::voice {

struct CachedMusicGeneration {
    std::string generationId;
    std::string scriptId;
    std::string title;
    std::string prompt;
    std::string generationMode;
    double durationSeconds{0.0};
    std::filesystem::path wavPath;
    WavProvenance provenance;
};

[[nodiscard]] Result<CachedMusicGeneration> saveMusicGeneration(const CachedMusicGeneration &generation,
                                                                const std::vector<uint8_t> &monoPcm);

[[nodiscard]] Result<CachedMusicGeneration> loadMusicGeneration(const std::string &generationId);

/// Immutable shareable rendition cache. A miss is a successful nullopt; the
/// controller encodes once through SoundRenditionService and stores the bytes.
[[nodiscard]] Result<std::optional<std::vector<uint8_t>>> loadMusicMp3(const std::string &generationId);
[[nodiscard]] Result<void> saveMusicMp3(const std::string &generationId, std::span<const uint8_t> bytes);

} // namespace creatures::voice
