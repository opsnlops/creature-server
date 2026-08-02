#include "server/voice/MusicGenerationCache.h"

#include <algorithm>
#include <chrono>
#include <fstream>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "server/storage/Storage.h"
#include "server/voice/IxmlReader.h"
#include "server/voice/PcmWavWriter.h"
#include "util/helpers.h"

namespace creatures::voice {

namespace {

constexpr const char *kMusicCacheSubdir = "music";
constexpr std::uintmax_t kMaxCandidateWavBytes = 80ULL * 1024 * 1024;
constexpr std::uintmax_t kMaxCandidateMetadataBytes = 2ULL * 1024 * 1024;
constexpr std::uintmax_t kMaxCandidateMp3Bytes = 32ULL * 1024 * 1024;
constexpr std::size_t kMaxMusicCandidates = 64;
constexpr std::uintmax_t kMaxMusicCacheBytes = 2ULL * 1024 * 1024 * 1024;

struct CandidateUsage {
    std::string generationId;
    std::filesystem::file_time_type modified;
    std::uintmax_t bytes{0};
};

Result<void> pruneMusicCache(std::uintmax_t incomingBytes) {
    auto root = storage::root(storage::Persistence::GenerationCache);
    if (!root.isSuccess()) {
        return Result<void>{root.getError().value()};
    }
    const auto directory = root.getValue().value() / kMusicCacheSubdir;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return Result<void>{
            ServerError(ServerError::InternalError, fmt::format("could not create music cache: {}", ec.message()))};
    }

    std::vector<CandidateUsage> candidates;
    std::uintmax_t totalBytes = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            return Result<void>{
                ServerError(ServerError::InternalError, fmt::format("could not scan music cache: {}", ec.message()))};
        }
        if (!entry.is_regular_file(ec) || ec || entry.path().extension() != ".wav" ||
            !isUuidShape(entry.path().stem().string())) {
            ec.clear();
            continue;
        }
        CandidateUsage usage;
        usage.generationId = entry.path().stem().string();
        usage.modified = entry.last_write_time(ec);
        if (ec) {
            return Result<void>{ServerError(ServerError::InternalError,
                                            fmt::format("could not inspect music cache: {}", ec.message()))};
        }
        for (const char *extension : {".wav", ".json", ".mp3"}) {
            const auto path = directory / (usage.generationId + extension);
            if (std::filesystem::exists(path, ec)) {
                usage.bytes += std::filesystem::file_size(path, ec);
            }
            if (ec) {
                return Result<void>{ServerError(ServerError::InternalError,
                                                fmt::format("could not measure music cache: {}", ec.message()))};
            }
        }
        totalBytes += usage.bytes;
        candidates.push_back(std::move(usage));
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateUsage &a, const CandidateUsage &b) { return a.modified < b.modified; });

    std::size_t removeCount = 0;
    while (removeCount < candidates.size() && (candidates.size() - removeCount >= kMaxMusicCandidates ||
                                               totalBytes + incomingBytes > kMaxMusicCacheBytes)) {
        const auto &candidate = candidates[removeCount++];
        for (const char *extension : {".wav", ".json", ".mp3"}) {
            std::filesystem::remove(directory / (candidate.generationId + extension), ec);
            if (ec) {
                return Result<void>{ServerError(ServerError::InternalError,
                                                fmt::format("could not prune music cache: {}", ec.message()))};
            }
        }
        totalBytes -= std::min(totalBytes, candidate.bytes);
    }
    if (totalBytes + incomingBytes > kMaxMusicCacheBytes) {
        return Result<void>{ServerError(ServerError::InvalidData, "music candidate exceeds cache capacity")};
    }
    return Result<void>{};
}

Result<std::filesystem::path> candidatePath(const std::string &generationId, const std::string &extension) {
    if (!isUuidShape(generationId)) {
        return Result<std::filesystem::path>{
            ServerError(ServerError::InvalidData, "music generation id must be a UUID")};
    }
    auto result = storage::allocateSoundPath(storage::Persistence::GenerationCache, generationId + extension,
                                             std::string(kMusicCacheSubdir));
    if (!result.isSuccess()) {
        return Result<std::filesystem::path>{result.getError().value()};
    }
    return Result<std::filesystem::path>{result.getValue().value().absolute};
}

} // namespace

Result<CachedMusicGeneration> saveMusicGeneration(const CachedMusicGeneration &generation,
                                                  const std::vector<uint8_t> &monoPcm) {
    if (!isUuidShape(generation.generationId) || !isUuidShape(generation.scriptId) || monoPcm.empty() ||
        !generation.provenance.music || generation.provenance.music->requestJson.empty()) {
        return Result<CachedMusicGeneration>{
            ServerError(ServerError::InvalidData, "music generation cache input is incomplete")};
    }
    auto pruned = pruneMusicCache(monoPcm.size() + kMaxCandidateMetadataBytes);
    if (!pruned.isSuccess()) {
        return Result<CachedMusicGeneration>{pruned.getError().value()};
    }
    const auto wav = wrapMonoPcmAsWav(monoPcm, 48000, &generation.provenance);
    auto wavWrite = storage::writeSoundFile(storage::Persistence::GenerationCache, generation.generationId + ".wav",
                                            wav, std::string(kMusicCacheSubdir));
    if (!wavWrite.isSuccess()) {
        return Result<CachedMusicGeneration>{wavWrite.getError().value()};
    }

    nlohmann::json metadata{{"generation_id", generation.generationId},
                            {"script_id", generation.scriptId},
                            {"title", generation.title},
                            {"prompt", generation.prompt},
                            {"generation_mode", generation.generationMode},
                            {"duration_seconds", generation.durationSeconds},
                            {"provenance", wavProvenanceToJson(generation.provenance)}};
    const auto metadataText = metadata.dump(2);
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t *>(metadataText.data()), metadataText.size());
    auto jsonWrite = storage::writeSoundFile(storage::Persistence::GenerationCache, generation.generationId + ".json",
                                             bytes, std::string(kMusicCacheSubdir));
    if (!jsonWrite.isSuccess()) {
        std::error_code ignored;
        std::filesystem::remove(wavWrite.getValue().value().absolute, ignored);
        return Result<CachedMusicGeneration>{jsonWrite.getError().value()};
    }
    auto saved = generation;
    saved.wavPath = wavWrite.getValue().value().absolute;
    return Result<CachedMusicGeneration>{std::move(saved)};
}

Result<CachedMusicGeneration> loadMusicGeneration(const std::string &generationId) {
    auto wavPath = candidatePath(generationId, ".wav");
    auto jsonPath = candidatePath(generationId, ".json");
    if (!wavPath.isSuccess())
        return Result<CachedMusicGeneration>{wavPath.getError().value()};
    if (!jsonPath.isSuccess())
        return Result<CachedMusicGeneration>{jsonPath.getError().value()};

    std::error_code ec;
    if (!std::filesystem::exists(wavPath.getValue().value(), ec) || ec ||
        !std::filesystem::exists(jsonPath.getValue().value(), ec) || ec) {
        return Result<CachedMusicGeneration>{ServerError(ServerError::NotFound, "music generation not found")};
    }
    const auto wavSize = std::filesystem::file_size(wavPath.getValue().value(), ec);
    if (ec || wavSize == 0 || wavSize > kMaxCandidateWavBytes) {
        return Result<CachedMusicGeneration>{
            ServerError(ServerError::InvalidData, "music generation WAV is empty or too large")};
    }
    const auto metadataSize = std::filesystem::file_size(jsonPath.getValue().value(), ec);
    if (ec || metadataSize == 0 || metadataSize > kMaxCandidateMetadataBytes) {
        return Result<CachedMusicGeneration>{
            ServerError(ServerError::InvalidData, "music generation metadata is empty or too large")};
    }

    try {
        nlohmann::json metadata;
        std::ifstream input(jsonPath.getValue().value());
        if (!input) {
            return Result<CachedMusicGeneration>{
                ServerError(ServerError::InternalError, "could not open music generation metadata")};
        }
        input >> metadata;
        CachedMusicGeneration generation;
        generation.generationId = metadata.value("generation_id", std::string{});
        generation.scriptId = metadata.value("script_id", std::string{});
        generation.title = metadata.value("title", std::string{});
        generation.prompt = metadata.value("prompt", std::string{});
        generation.generationMode = metadata.value("generation_mode", std::string{});
        generation.durationSeconds = metadata.value("duration_seconds", 0.0);
        generation.provenance = wavProvenanceFromJson(metadata.value("provenance", nlohmann::json::object()));
        generation.wavPath = wavPath.getValue().value();
        if (generation.generationId != generationId || !isUuidShape(generation.scriptId) || generation.prompt.empty() ||
            !generation.provenance.music || generation.provenance.music->musicGenerationId != generation.generationId) {
            return Result<CachedMusicGeneration>{
                ServerError(ServerError::InvalidData, "music generation metadata failed validation")};
        }
        return Result<CachedMusicGeneration>{std::move(generation)};
    } catch (const std::exception &e) {
        return Result<CachedMusicGeneration>{
            ServerError(ServerError::InvalidData, fmt::format("music generation metadata is invalid: {}", e.what()))};
    }
}

Result<std::optional<std::vector<uint8_t>>> loadMusicMp3(const std::string &generationId) {
    auto path = candidatePath(generationId, ".mp3");
    if (!path.isSuccess()) {
        return Result<std::optional<std::vector<uint8_t>>>{path.getError().value()};
    }
    std::error_code ec;
    if (!std::filesystem::exists(path.getValue().value(), ec)) {
        if (ec) {
            return Result<std::optional<std::vector<uint8_t>>>{
                ServerError(ServerError::InternalError, "could not inspect cached music MP3")};
        }
        return Result<std::optional<std::vector<uint8_t>>>{std::optional<std::vector<uint8_t>>{}};
    }
    const auto size = std::filesystem::file_size(path.getValue().value(), ec);
    if (ec || size == 0 || size > kMaxCandidateMp3Bytes) {
        return Result<std::optional<std::vector<uint8_t>>>{
            ServerError(ServerError::InvalidData, "cached music MP3 is empty or too large")};
    }
    std::ifstream input(path.getValue().value(), std::ios::binary);
    if (!input) {
        return Result<std::optional<std::vector<uint8_t>>>{
            ServerError(ServerError::InternalError, "could not open cached music MP3")};
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return Result<std::optional<std::vector<uint8_t>>>{
            ServerError(ServerError::InternalError, "could not read cached music MP3")};
    }
    return Result<std::optional<std::vector<uint8_t>>>{std::optional<std::vector<uint8_t>>(std::move(bytes))};
}

Result<void> saveMusicMp3(const std::string &generationId, std::span<const uint8_t> bytes) {
    if (!isUuidShape(generationId) || bytes.empty() || bytes.size() > kMaxCandidateMp3Bytes) {
        return Result<void>{ServerError(ServerError::InvalidData, "cached music MP3 input is invalid")};
    }
    auto write = storage::writeSoundFile(storage::Persistence::GenerationCache, generationId + ".mp3", bytes,
                                         std::string(kMusicCacheSubdir));
    if (!write.isSuccess()) {
        return Result<void>{write.getError().value()};
    }
    return Result<void>{};
}

} // namespace creatures::voice
