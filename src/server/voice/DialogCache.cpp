#include "DialogCache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "server/namespace-stuffs.h"

namespace creatures::voice {

namespace {

/// Root of the on-disk dialog cache. Lives under the ad-hoc temp tree so the
/// existing cron sweep cleans up old generations automatically — no TTL/LRU
/// bookkeeping in-process.
std::filesystem::path dialogCacheRoot() {
    return std::filesystem::temp_directory_path() / "creature-adhoc" / "dialog-cache";
}

std::filesystem::path cacheKeyDir(const std::string &cacheKey) { return dialogCacheRoot() / cacheKey; }

std::filesystem::path pcmPath(const std::string &cacheKey, const std::string &generationId) {
    return cacheKeyDir(cacheKey) / (generationId + ".pcm");
}
std::filesystem::path jsonPath(const std::string &cacheKey, const std::string &generationId) {
    return cacheKeyDir(cacheKey) / (generationId + ".json");
}

/// Sha-256 over `data`, returned as 64-char lowercase hex.
std::string sha256Hex(const std::string &data) {
    std::array<uint8_t, EVP_MAX_MD_SIZE> hash{};
    unsigned int hashLen = 0;

    auto ctxDeleter = [](EVP_MD_CTX *c) {
        if (c) {
            EVP_MD_CTX_free(c);
        }
    };
    std::unique_ptr<EVP_MD_CTX, decltype(ctxDeleter)> ctx(EVP_MD_CTX_new(), ctxDeleter);
    if (!ctx) {
        // Shouldn't fail; if it does, return a marker so callers don't
        // silently collide on an empty string.
        return std::string{};
    }
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        return std::string{};
    }
    if (EVP_DigestUpdate(ctx.get(), data.data(), data.size()) != 1) {
        return std::string{};
    }
    if (EVP_DigestFinal_ex(ctx.get(), hash.data(), &hashLen) != 1 || hashLen == 0) {
        return std::string{};
    }
    std::string hex;
    hex.reserve(hashLen * 2);
    for (unsigned int i = 0; i < hashLen; ++i) {
        constexpr const char *digits = "0123456789abcdef";
        hex.push_back(digits[(hash[i] >> 4) & 0xF]);
        hex.push_back(digits[hash[i] & 0xF]);
    }
    return hex;
}

/// Convert a system_clock::time_point to ISO-8601 string (UTC). Matches
/// `formatTimeISO8601` in util/helpers (which we'd have a small dependency
/// chain to pull in just for this; inline it).
std::string toIso8601(std::chrono::system_clock::time_point tp) {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                       tm.tm_min, tm.tm_sec);
}

std::chrono::system_clock::time_point fromIso8601(const std::string &iso) {
    std::tm tm{};
    int y = 1970, mo = 1, d = 1, h = 0, mi = 0, s = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &s) != 6) {
        return std::chrono::system_clock::time_point{};
    }
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    // timegm is the inverse of gmtime; not in POSIX but on macOS/Linux it's
    // available. Use _mkgmtime on Windows if we ever build there.
#if defined(_WIN32)
    const std::time_t t = _mkgmtime(&tm);
#else
    const std::time_t t = timegm(&tm);
#endif
    return std::chrono::system_clock::from_time_t(t);
}

nlohmann::json voiceSegmentsToJson(const std::vector<DialogVoiceSegment> &segments) {
    auto arr = nlohmann::json::array();
    for (const auto &s : segments) {
        arr.push_back({{"voice_id", s.voiceId},
                       {"character_start_index", s.characterStartIndex},
                       {"character_end_index", s.characterEndIndex},
                       {"dialog_input_index", s.dialogInputIndex},
                       {"start_time_seconds", s.startTimeSeconds},
                       {"end_time_seconds", s.endTimeSeconds}});
    }
    return arr;
}

std::vector<DialogVoiceSegment> voiceSegmentsFromJson(const nlohmann::json &arr) {
    std::vector<DialogVoiceSegment> out;
    if (!arr.is_array()) {
        return out;
    }
    out.reserve(arr.size());
    for (const auto &j : arr) {
        DialogVoiceSegment s;
        s.voiceId = j.value("voice_id", std::string{});
        s.characterStartIndex = j.value("character_start_index", std::size_t{0});
        s.characterEndIndex = j.value("character_end_index", std::size_t{0});
        s.dialogInputIndex = j.value("dialog_input_index", std::size_t{0});
        s.startTimeSeconds = j.value("start_time_seconds", 0.0);
        s.endTimeSeconds = j.value("end_time_seconds", 0.0);
        out.push_back(std::move(s));
    }
    return out;
}

nlohmann::json forcedAlignmentToJson(const ForcedAlignmentResult &fa) {
    auto words = nlohmann::json::array();
    for (const auto &w : fa.words) {
        words.push_back({{"text", w.text}, {"start", w.startSeconds}, {"end", w.endSeconds}});
    }
    auto chars = nlohmann::json::array();
    for (const auto &c : fa.characters) {
        chars.push_back({{"text", c.text}, {"start", c.startSeconds}, {"end", c.endSeconds}});
    }
    return {{"loss", fa.loss}, {"words", std::move(words)}, {"characters", std::move(chars)}};
}

ForcedAlignmentResult forcedAlignmentFromJson(const nlohmann::json &j) {
    ForcedAlignmentResult fa;
    fa.loss = j.value("loss", 0.0);
    if (j.contains("words") && j["words"].is_array()) {
        for (const auto &w : j["words"]) {
            ForcedAlignmentWord word;
            word.text = w.value("text", std::string{});
            word.startSeconds = w.value("start", 0.0);
            word.endSeconds = w.value("end", 0.0);
            fa.words.push_back(std::move(word));
        }
    }
    if (j.contains("characters") && j["characters"].is_array()) {
        for (const auto &c : j["characters"]) {
            ForcedAlignmentChar ch;
            ch.text = c.value("text", std::string{});
            ch.startSeconds = c.value("start", 0.0);
            ch.endSeconds = c.value("end", 0.0);
            fa.characters.push_back(std::move(ch));
        }
    }
    return fa;
}

/// Read a whole file into a byte vector. Empty vector on failure.
std::vector<uint8_t> readBinary(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

std::string computeCacheKey(const std::vector<DialogInput> &turns) {
    // Stable JSON serialization keyed by short field names so the hash doesn't
    // shift if someone renames a struct field. Model identifier is prefixed
    // so a future model bump cleanly invalidates the cache for these inputs.
    auto arr = nlohmann::json::array();
    for (const auto &t : turns) {
        arr.push_back({{"v", t.voiceId}, {"t", t.text}});
    }
    std::string serialized = "eleven_v3|" + arr.dump();
    return sha256Hex(serialized);
}

std::vector<GenerationListEntry> listGenerations(const std::string &cacheKey) {
    std::vector<GenerationListEntry> out;
    const auto dir = cacheKeyDir(cacheKey);
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return out;
    }
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto p = entry.path();
        if (p.extension() != ".json") {
            continue;
        }
        // Need the matching .pcm to count as a complete generation — a stray
        // .json without audio is treated as not-present.
        const auto pcm = p;
        auto pcmAlt = pcm;
        pcmAlt.replace_extension(".pcm");
        if (!std::filesystem::exists(pcmAlt, ec)) {
            continue;
        }
        GenerationListEntry e;
        e.generationId = p.stem().string();
        const auto ft = std::filesystem::last_write_time(p, ec);
        if (ec) {
            continue;
        }
        // file_time_type → system_clock::time_point. Conversion is
        // implementation-defined; the clock_cast in C++20 is the right tool
        // but isn't universally available — and we only need it for sorting,
        // so a duration cast is fine.
        const auto sysTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - decltype(ft)::clock::now() + std::chrono::system_clock::now());
        e.createdAt = sysTime;
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.createdAt > b.createdAt; });
    return out;
}

std::optional<std::string> findLatestGeneration(const std::string &cacheKey) {
    const auto gens = listGenerations(cacheKey);
    if (gens.empty()) {
        return std::nullopt;
    }
    return gens.front().generationId;
}

Result<CachedGeneration> loadGeneration(const std::string &cacheKey, const std::string &generationId) {
    const auto pcmFile = pcmPath(cacheKey, generationId);
    const auto jsonFile = jsonPath(cacheKey, generationId);
    std::error_code ec;
    if (!std::filesystem::exists(pcmFile, ec) || !std::filesystem::exists(jsonFile, ec)) {
        return Result<CachedGeneration>{ServerError(
            ServerError::NotFound, fmt::format("DialogCache: no generation '{}/{}' on disk", cacheKey, generationId))};
    }

    auto audio = readBinary(pcmFile);
    if (audio.empty()) {
        return Result<CachedGeneration>{ServerError(
            ServerError::InvalidData,
            fmt::format("DialogCache: generation '{}/{}' PCM file is empty or unreadable", cacheKey, generationId))};
    }

    nlohmann::json meta;
    try {
        std::ifstream in(jsonFile);
        in >> meta;
    } catch (const std::exception &e) {
        return Result<CachedGeneration>{ServerError(
            ServerError::InvalidData, fmt::format("DialogCache: generation '{}/{}' metadata parse failed: {}", cacheKey,
                                                  generationId, e.what()))};
    }

    CachedGeneration gen;
    gen.generationId = meta.value("generation_id", generationId);
    gen.turnsSummary = meta.value("turns_summary", std::string{});
    if (meta.contains("created_at") && meta["created_at"].is_string()) {
        gen.createdAt = fromIso8601(meta["created_at"].get<std::string>());
    } else {
        gen.createdAt = std::chrono::system_clock::now();
    }
    if (meta.contains("voice_segments")) {
        gen.voiceSegments = voiceSegmentsFromJson(meta["voice_segments"]);
    }
    if (meta.contains("forced_alignment")) {
        gen.forcedAlignment = forcedAlignmentFromJson(meta["forced_alignment"]);
    }
    gen.audioPcm = std::move(audio);
    return gen;
}

Result<void> saveGeneration(const std::string &cacheKey, const CachedGeneration &gen) {
    if (cacheKey.empty()) {
        return Result<void>{ServerError(ServerError::InvalidData, "DialogCache: cacheKey is empty")};
    }
    if (gen.generationId.empty()) {
        return Result<void>{ServerError(ServerError::InvalidData, "DialogCache: generation id is empty")};
    }
    if (gen.audioPcm.empty()) {
        return Result<void>{ServerError(ServerError::InvalidData, "DialogCache: generation audio is empty")};
    }

    const auto dir = cacheKeyDir(cacheKey);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return Result<void>{ServerError(ServerError::InternalError,
                                        fmt::format("DialogCache: mkdir {} failed: {}", dir.string(), ec.message()))};
    }

    // Write each file to a .tmp sibling then rename. This way an interrupted
    // save can't leave a half-written file that loadGeneration would mistake
    // for valid. (listGenerations also requires both files to exist before
    // counting the generation, as a belt + braces check.)
    const auto pcmFile = pcmPath(cacheKey, gen.generationId);
    const auto jsonFile = jsonPath(cacheKey, gen.generationId);
    const auto pcmTmp = pcmFile.string() + ".tmp";
    const auto jsonTmp = jsonFile.string() + ".tmp";

    {
        std::ofstream out(pcmTmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return Result<void>{
                ServerError(ServerError::InternalError, fmt::format("DialogCache: open {} for write failed", pcmTmp))};
        }
        out.write(reinterpret_cast<const char *>(gen.audioPcm.data()),
                  static_cast<std::streamsize>(gen.audioPcm.size()));
        out.flush();
        if (!out) {
            return Result<void>{
                ServerError(ServerError::InternalError, fmt::format("DialogCache: write {} failed", pcmTmp))};
        }
    }

    nlohmann::json meta;
    meta["generation_id"] = gen.generationId;
    meta["created_at"] = toIso8601(gen.createdAt);
    meta["turns_summary"] = gen.turnsSummary;
    meta["voice_segments"] = voiceSegmentsToJson(gen.voiceSegments);
    meta["forced_alignment"] = forcedAlignmentToJson(gen.forcedAlignment);

    {
        std::ofstream out(jsonTmp, std::ios::trunc);
        if (!out) {
            std::filesystem::remove(pcmTmp, ec);
            return Result<void>{
                ServerError(ServerError::InternalError, fmt::format("DialogCache: open {} for write failed", jsonTmp))};
        }
        out << meta.dump(2);
        out.flush();
        if (!out) {
            std::filesystem::remove(pcmTmp, ec);
            return Result<void>{
                ServerError(ServerError::InternalError, fmt::format("DialogCache: write {} failed", jsonTmp))};
        }
    }

    // Atomic rename of both. Filesystem semantics: each rename is atomic on
    // its own; if the second one fails after the first succeeds, we'd have a
    // .pcm without a .json — listGenerations' "needs both files" rule
    // protects loaders from treating that as valid.
    std::filesystem::rename(pcmTmp, pcmFile, ec);
    if (ec) {
        std::filesystem::remove(pcmTmp, ec);
        std::filesystem::remove(jsonTmp, ec);
        return Result<void>{ServerError(ServerError::InternalError,
                                        fmt::format("DialogCache: rename {} → {} failed", pcmTmp, pcmFile.string()))};
    }
    std::filesystem::rename(jsonTmp, jsonFile, ec);
    if (ec) {
        std::filesystem::remove(jsonTmp, ec);
        return Result<void>{ServerError(ServerError::InternalError,
                                        fmt::format("DialogCache: rename {} → {} failed", jsonTmp, jsonFile.string()))};
    }

    debug("DialogCache: saved generation {}/{} ({} audio bytes, {} segments, {} words)", cacheKey, gen.generationId,
          gen.audioPcm.size(), gen.voiceSegments.size(), gen.forcedAlignment.words.size());
    return Result<void>{};
}

} // namespace creatures::voice
