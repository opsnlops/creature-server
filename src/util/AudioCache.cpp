//
// AudioCache.cpp - Fast Opus file caching implementation
//

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unistd.h>

// For SHA-256 hashing - we'll use OpenSSL since it's likely already linked
#include <openssl/evp.h>
#include <openssl/sha.h>

// For OGG/Opus file handling
#include <opus/opus.h>
#include <opus/opusfile.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "AudioCache.h"
#include "JsonParser.h"
#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

using namespace creatures;
using namespace creatures::util;

AudioCache::AudioCache(const std::string &soundDirectory) : soundDirectory_(soundDirectory) {

    // Get hostname for per-machine cache isolation
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        // Fallback to a default if hostname retrieval fails
        strncpy(hostname, "unknown-host", sizeof(hostname) - 1);
        hostname[sizeof(hostname) - 1] = '\0';
    }

    // Create cache directory as a subdirectory of the sounds directory with hostname
    cacheDirectory_ = std::filesystem::path(soundDirectory_) / ".opus_cache" / hostname;

    // Ensure cache directory exists and is writable
    auto result = ensureCacheDirectoryWritable();
    if (!result.isSuccess()) {
        throw std::runtime_error(fmt::format("Failed to initialize audio cache: {}", result.getError()->getMessage()));
    }

    debug("AudioCache initialized in directory: {} (hostname: {})", cacheDirectory_, hostname);
}

std::shared_ptr<AudioCache::CachedAudioData> AudioCache::tryLoadFromCache(const std::string &sourceFilePath,
                                                                          std::shared_ptr<OperationSpan> parentSpan) {

    auto span =
        observability ? observability->createChildOperationSpan("AudioCache.tryLoadFromCache", parentSpan) : nullptr;

    if (span) {
        span->setAttribute("source_file", sourceFilePath);
    }

    try {
        // Fast check: do all cache files exist?
        if (!allCacheFilesExist(sourceFilePath)) {
            cacheMisses_++;
            if (span)
                span->setAttribute("cache_result", "miss_files_missing");
            debug("Cache miss: files missing for {}", sourceFilePath);
            return nullptr;
        }

        // Get current source file info for validation
        auto sourceInfoResult = getSourceFileInfo(sourceFilePath);
        if (!sourceInfoResult.isSuccess()) {
            cacheMisses_++;
            if (span) {
                span->setAttribute("cache_result", "miss_source_info_error");
                span->setError(sourceInfoResult.getError()->getMessage());
            }
            warn("Cache miss: failed to get source file info for {}: {}", sourceFilePath,
                 sourceInfoResult.getError()->getMessage());
            return nullptr;
        }

        auto currentSourceInfo = sourceInfoResult.getValue().value();

        // Load and validate the first channel file (they should all have the same metadata)
        auto channel0Path = getCacheFilePath(sourceFilePath, 0);
        auto loadResult = loadOggOpusWithMetadata(channel0Path);
        if (!loadResult.isSuccess()) {
            cacheMisses_++;
            if (span) {
                span->setAttribute("cache_result", "miss_load_error");
                span->setError(loadResult.getError()->getMessage());
            }
            debug("Cache miss: failed to load channel 0 cache file: {}", loadResult.getError()->getMessage());
            return nullptr;
        }

        auto [firstChannelFrames, cachedSourceInfo] = loadResult.getValue().value();

        // Validate that source file hasn't changed
        if (!(currentSourceInfo == cachedSourceInfo)) {
            cacheMisses_++;
            if (span)
                span->setAttribute("cache_result", "miss_file_changed");
            debug("Cache miss: source file has changed for {}", sourceFilePath);
            // Clear stale cache
            clearCache(sourceFilePath);
            return nullptr;
        }

        // Source file is valid! Load all channels
        auto cachedData = std::make_shared<CachedAudioData>();
        cachedData->framesPerChannel = firstChannelFrames.size();
        cachedData->encodedFrames[0] = std::move(firstChannelFrames);

        // Load remaining channels (1-16)
        for (uint8_t ch = 1; ch < RTP_STREAMING_CHANNELS; ++ch) {
            auto channelPath = getCacheFilePath(sourceFilePath, ch);
            auto channelResult = loadOggOpusWithMetadata(channelPath);
            if (!channelResult.isSuccess()) {
                cacheMisses_++;
                if (span) {
                    span->setAttribute("cache_result", "miss_channel_load_error");
                    span->setError(
                        fmt::format("Channel {} load failed: {}", ch, channelResult.getError()->getMessage()));
                }
                warn("Cache miss: failed to load channel {} for {}: {}", ch, sourceFilePath,
                     channelResult.getError()->getMessage());
                return nullptr;
            }

            auto [channelFrames, channelSourceInfo] = channelResult.getValue().value();

            // Sanity check: all channels should have same frame count and source info
            if (channelFrames.size() != cachedData->framesPerChannel || !(channelSourceInfo == currentSourceInfo)) {
                cacheMisses_++;
                if (span)
                    span->setAttribute("cache_result", "miss_inconsistent_channels");
                warn("Cache miss: inconsistent channel data for {}", sourceFilePath);
                clearCache(sourceFilePath); // Clear corrupted cache
                return nullptr;
            }

            cachedData->encodedFrames[ch] = std::move(channelFrames);
        }

        cacheHits_++;
        if (span) {
            span->setAttribute("cache_result", "hit");
            span->setAttribute("frames_loaded", static_cast<int64_t>(cachedData->framesPerChannel));
            span->setSuccess();
        }

        debug("Cache hit: loaded {} frames from cache for {}", cachedData->framesPerChannel, sourceFilePath);
        return cachedData;

    } catch (const std::exception &e) {
        cacheMisses_++;
        if (span) {
            span->setAttribute("cache_result", "miss_exception");
            span->setError(e.what());
        }
        error("Cache miss due to exception for {}: {}", sourceFilePath, e.what());
        return nullptr;
    }
}

Result<void> AudioCache::saveToCache(const std::string &sourceFilePath, const CachedAudioData &audioData,
                                     std::shared_ptr<OperationSpan> parentSpan) {

    auto span = observability ? observability->createChildOperationSpan("AudioCache.saveToCache", parentSpan) : nullptr;

    if (span) {
        span->setAttribute("source_file", sourceFilePath);
        span->setAttribute("frames_to_save", static_cast<int64_t>(audioData.framesPerChannel));
    }

    try {
        // Get source file info for metadata
        auto sourceInfoResult = getSourceFileInfo(sourceFilePath);
        if (!sourceInfoResult.isSuccess()) {
            if (span)
                span->setError(sourceInfoResult.getError()->getMessage());
            return Result<void>(sourceInfoResult.getError().value());
        }

        auto sourceInfo = sourceInfoResult.getValue().value();

        // Ensure cache directory exists
        auto cacheDir = getCacheDirectoryPath(sourceFilePath);
        std::filesystem::create_directories(cacheDir);

        // Save each channel as a separate OGG Opus file
        for (uint8_t ch = 0; ch < RTP_STREAMING_CHANNELS; ++ch) {
            auto cachePath = getCacheFilePath(sourceFilePath, ch);
            auto saveResult = saveAsOggOpusWithMetadata(cachePath, audioData.encodedFrames[ch], sourceInfo);
            if (!saveResult.isSuccess()) {
                if (span)
                    span->setError(saveResult.getError()->getMessage());
                // Clean up partially written cache
                clearCache(sourceFilePath);
                return saveResult;
            }
        }

        if (span)
            span->setSuccess();
        info("Saved {} frames to cache for {}", audioData.framesPerChannel, sourceFilePath);
        return Result<void>();

    } catch (const std::exception &e) {
        auto errorMsg = fmt::format("Exception while saving to cache: {}", e.what());
        if (span)
            span->setError(errorMsg);
        error(errorMsg);
        return Result<void>(ServerError(ServerError::InternalError, errorMsg));
    }
}

Result<void> AudioCache::clearCache(const std::string &sourceFilePath) {
    try {
        auto cacheDir = getCacheDirectoryPath(sourceFilePath);
        if (std::filesystem::exists(cacheDir)) {
            std::filesystem::remove_all(cacheDir);
            debug("Cleared cache for {}", sourceFilePath);
        }
        return Result<void>();
    } catch (const std::exception &e) {
        auto errorMsg = fmt::format("Failed to clear cache for {}: {}", sourceFilePath, e.what());
        error(errorMsg);
        return Result<void>(ServerError(ServerError::InternalError, errorMsg));
    }
}

AudioCache::CacheStats AudioCache::getStats() const {
    CacheStats stats{};
    stats.cacheHits = cacheHits_;
    stats.cacheMisses = cacheMisses_;

    try {
        if (std::filesystem::exists(cacheDirectory_)) {
            for (const auto &entry : std::filesystem::recursive_directory_iterator(cacheDirectory_)) {
                if (entry.is_regular_file()) {
                    stats.totalCachedFiles++;
                    stats.totalDiskUsage += entry.file_size();
                }
            }
        }
    } catch (const std::exception &e) {
        warn("Failed to calculate cache stats: {}", e.what());
    }

    return stats;
}

// Private implementation methods

std::string AudioCache::getCacheFilePath(const std::string &sourceFilePath, uint8_t channel) const {
    auto cacheDirPath = getCacheDirectoryPath(sourceFilePath);
    auto filename = fmt::format("ch{:02d}.opus", channel);
    return (std::filesystem::path(cacheDirPath) / filename).string();
}

namespace {
std::string sanitizeComponent(const std::string &component) {
    std::string sanitized;
    sanitized.reserve(component.size());
    for (char c : component) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
            sanitized += c;
        } else {
            sanitized += '_';
        }
    }

    if (sanitized.empty() || sanitized[0] == '.') {
        sanitized = "file_" + std::to_string(std::hash<std::string>{}(component));
    }

    constexpr size_t MAX_FILENAME_LENGTH = 200;
    if (sanitized.length() > MAX_FILENAME_LENGTH) {
        auto hash = std::to_string(std::hash<std::string>{}(sanitized));
        sanitized = sanitized.substr(0, MAX_FILENAME_LENGTH - hash.length() - 1) + "_" + hash;
    }
    return sanitized;
}
} // namespace

std::string AudioCache::getCacheDirectoryPath(const std::string &sourceFilePath) const {
    try {
        // Canonicalize the source path to resolve any .. or . components
        auto canonicalSourcePath = std::filesystem::canonical(sourceFilePath);

        bool insideSoundDir = false;
        try {
            auto canonicalSoundDir = std::filesystem::canonical(soundDirectory_);
            auto relativePath = std::filesystem::relative(canonicalSourcePath, canonicalSoundDir);
            auto relativePathStr = relativePath.string();
            if (!relativePath.empty() && !(relativePathStr.size() >= 2 && relativePathStr.substr(0, 2) == "..")) {
                insideSoundDir = true;
            }
        } catch (const std::exception &e) {
            warn("Unable to evaluate relative path for caching ({}): {}", soundDirectory_, e.what());
        }

        auto filename = canonicalSourcePath.stem().string();
        auto sanitized = sanitizeComponent(filename);

        std::filesystem::path baseCachePath = cacheDirectory_;
        if (!insideSoundDir) {
            baseCachePath /= "_external";
            sanitized = fmt::format("{}_{}", sanitized, std::hash<std::string>{}(canonicalSourcePath.string()));
        }

        return (baseCachePath / sanitized).string();

    } catch (const std::filesystem::filesystem_error &e) {
        error("Filesystem error processing cache path for {}: {}", sourceFilePath, e.what());
        throw std::invalid_argument("Invalid file path for caching");
    } catch (const std::exception &e) {
        error("Error processing cache path for {}: {}", sourceFilePath, e.what());
        throw;
    }
}

Result<AudioCache::SourceFileInfo> AudioCache::getSourceFileInfo(const std::string &filePath) const {
    try {
        if (!std::filesystem::exists(filePath)) {
            return Result<SourceFileInfo>{
                ServerError(ServerError::NotFound, fmt::format("Source file not found: {}", filePath))};
        }

        auto fileStatus = std::filesystem::status(filePath);
        if (!std::filesystem::is_regular_file(fileStatus)) {
            return Result<SourceFileInfo>{
                ServerError(ServerError::InvalidData, fmt::format("Source path is not a regular file: {}", filePath))};
        }

        SourceFileInfo info;
        info.filePath = filePath;
        info.modTime = std::filesystem::last_write_time(filePath);
        info.fileSize = std::filesystem::file_size(filePath);

        auto checksumResult = calculateFileChecksum(filePath);
        if (!checksumResult.isSuccess()) {
            return Result<SourceFileInfo>{checksumResult.getError().value()};
        }
        info.checksum = checksumResult.getValue().value();

        return Result<SourceFileInfo>{info};

    } catch (const std::exception &e) {
        return Result<SourceFileInfo>{
            ServerError(ServerError::InternalError, fmt::format("Failed to get source file info: {}", e.what()))};
    }
}

Result<std::string> AudioCache::calculateFileChecksum(const std::string &filePath) const {
    // RAII wrapper for EVP_MD_CTX to ensure cleanup
    struct EVPContextGuard {
        EVP_MD_CTX *ctx;

        EVPContextGuard() : ctx(EVP_MD_CTX_new()) {}

        ~EVPContextGuard() {
            if (ctx) {
                EVP_MD_CTX_free(ctx);
            }
        }

        EVP_MD_CTX *get() const { return ctx; }
        operator bool() const { return ctx != nullptr; }
    };

    try {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            return Result<std::string>{
                ServerError(ServerError::Forbidden, fmt::format("Cannot open file for checksum: {}", filePath))};
        }

        EVPContextGuard context;
        if (!context) {
            return Result<std::string>{ServerError(ServerError::InternalError, "Failed to create hash context")};
        }

        if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
            return Result<std::string>{ServerError(ServerError::InternalError, "Failed to initialize hash")};
        }

        constexpr size_t BUFFER_SIZE = 8192;
        char buffer[BUFFER_SIZE];
        while (file.read(buffer, BUFFER_SIZE) || file.gcount() > 0) {
            const auto bytesRead = file.gcount();
            if (bytesRead <= 0)
                break;

            if (EVP_DigestUpdate(context.get(), buffer, static_cast<size_t>(bytesRead)) != 1) {
                return Result<std::string>{ServerError(ServerError::InternalError, "Failed to update hash")};
            }
        }

        // Check for file read errors
        if (file.bad()) {
            return Result<std::string>{ServerError(ServerError::InternalError,
                                                   fmt::format("Error reading file during checksum: {}", filePath))};
        }

        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hashLen = 0;
        if (EVP_DigestFinal_ex(context.get(), hash, &hashLen) != 1 || hashLen == 0) {
            return Result<std::string>{ServerError(ServerError::InternalError, "Failed to finalize hash")};
        }

        // Convert to hex string with bounds checking
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < hashLen && i < EVP_MAX_MD_SIZE; ++i) {
            ss << std::setw(2) << static_cast<unsigned>(hash[i]);
        }

        std::string result = ss.str();
        if (result.empty()) {
            return Result<std::string>{ServerError(ServerError::InternalError, "Generated empty hash")};
        }

        return Result<std::string>{result};

    } catch (const std::exception &e) {
        return Result<std::string>{ServerError(ServerError::InternalError,
                                               fmt::format("Exception during checksum calculation: {}", e.what()))};
    }
}

Result<std::pair<std::vector<std::vector<uint8_t>>, AudioCache::SourceFileInfo>>
AudioCache::loadOggOpusWithMetadata(const std::string &oggFilePath) const {
    // For now, implement a simple file format:
    // - Standard OGG Opus file with audio data
    // - Metadata stored in OGG comments/tags
    //
    // This is a simplified implementation - in production you'd want to use
    // libogg and libopus properly for full OGG Opus support

    try {
        std::ifstream file(oggFilePath, std::ios::binary);
        if (!file.is_open()) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                ServerError(ServerError::NotFound, fmt::format("Cache file not found: {}", oggFilePath))};
        }

        // For this implementation, we'll use a simple binary format:
        // [metadata_size:4][metadata_json][frame_count:4][frame_sizes:frame_count*4][frame_data...]

        uint32_t metadataSize;
        file.read(reinterpret_cast<char *>(&metadataSize), sizeof(metadataSize));
        if (!file.good()) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                ServerError(ServerError::InvalidData, "Failed to read metadata size")};
        }

        // Validate metadata size to prevent memory exhaustion
        constexpr uint32_t MAX_METADATA_SIZE = 64 * 1024; // 64KB should be more than enough
        if (metadataSize == 0) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                ServerError(ServerError::InvalidData, "Metadata size is zero")};
        }
        if (metadataSize > MAX_METADATA_SIZE) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                ServerError(ServerError::InvalidData,
                            fmt::format("Metadata size {} exceeds maximum {} bytes", metadataSize, MAX_METADATA_SIZE))};
        }

        std::string metadataJson;
        try {
            metadataJson.resize(metadataSize);
        } catch (const std::bad_alloc &e) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                ServerError(ServerError::InternalError,
                            fmt::format("Failed to allocate {} bytes for metadata: {}", metadataSize, e.what()))};
        }

        file.read(metadataJson.data(), metadataSize);
        if (!file.good() || static_cast<uint32_t>(file.gcount()) != metadataSize) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                ServerError(ServerError::InvalidData, fmt::format("Failed to read metadata: expected {} bytes, got {}",
                                                                  metadataSize, file.gcount()))};
        }

        // Parse metadata from JSON safely
        SourceFileInfo sourceInfo;
        auto jsonResult = JsonParser::parseJsonString(metadataJson, "audio cache metadata");
        if (!jsonResult.isSuccess()) {
            auto error = jsonResult.getError().value();
            warn("Failed to parse audio cache metadata: {}", error.getMessage());
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{error};
        }
        try {
            auto jsonData = jsonResult.getValue().value();
            sourceInfo.filePath = jsonData["path"];
            sourceInfo.fileSize = jsonData["size"];
            sourceInfo.checksum = jsonData["checksum"];

            // Get current modification time for the cached file path
            // (we don't store modTime in JSON since it's not easily serializable)
            if (std::filesystem::exists(sourceInfo.filePath)) {
                sourceInfo.modTime = std::filesystem::last_write_time(sourceInfo.filePath);
            } else {
                return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                    ServerError(ServerError::NotFound,
                                fmt::format("Cached source file no longer exists: {}", sourceInfo.filePath))};
            }
        } catch (const std::exception &e) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{ServerError(
                ServerError::InvalidData, fmt::format("Failed to parse cache metadata JSON: {}", e.what()))};
        }

        uint32_t frameCount;
        file.read(reinterpret_cast<char *>(&frameCount), sizeof(frameCount));
        if (!file.good()) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                ServerError(ServerError::InvalidData, "Failed to read frame count")};
        }

        // Validate frame count to prevent memory exhaustion attacks
        constexpr uint32_t MAX_FRAME_COUNT = 2000000; // ~5.5 hours at 48kHz/20ms frames
        if (frameCount == 0) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                ServerError(ServerError::InvalidData, "Frame count is zero")};
        }
        if (frameCount > MAX_FRAME_COUNT) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                ServerError(ServerError::InvalidData,
                            fmt::format("Frame count {} exceeds maximum {} frames", frameCount, MAX_FRAME_COUNT))};
        }

        // Check for potential overflow in allocation size (only if SIZE_MAX allows it)
        if constexpr (SIZE_MAX / sizeof(uint32_t) / 2 < UINT32_MAX) {
            constexpr size_t MAX_ALLOCATION_SIZE = SIZE_MAX / sizeof(uint32_t) / 2;
            if (frameCount > MAX_ALLOCATION_SIZE) {
                return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                    ServerError(ServerError::InvalidData,
                                fmt::format("Frame count {} would cause allocation overflow", frameCount))};
            }
        }

        std::vector<uint32_t> frameSizes;
        try {
            frameSizes.resize(frameCount);
        } catch (const std::bad_alloc &e) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                ServerError(ServerError::InternalError,
                            fmt::format("Failed to allocate memory for {} frame sizes: {}", frameCount, e.what()))};
        }

        const size_t bytesToRead = frameCount * sizeof(uint32_t);
        file.read(reinterpret_cast<char *>(frameSizes.data()), bytesToRead);
        if (!file.good() || static_cast<size_t>(file.gcount()) != bytesToRead) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{ServerError(
                ServerError::InvalidData,
                fmt::format("Failed to read frame sizes: expected {} bytes, got {}", bytesToRead, file.gcount()))};
        }

        std::vector<std::vector<uint8_t>> frames;
        try {
            frames.resize(frameCount);
        } catch (const std::bad_alloc &e) {
            return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                ServerError(ServerError::InternalError,
                            fmt::format("Failed to allocate memory for {} frames: {}", frameCount, e.what()))};
        }

        // Validate and load individual frame data with size limits
        constexpr uint32_t MAX_FRAME_SIZE = 8192; // 8KB per frame should be more than enough for Opus
        size_t totalDataSize = 0;
        constexpr size_t MAX_TOTAL_DATA_SIZE = 512 * 1024 * 1024; // 512MB total data limit

        for (uint32_t i = 0; i < frameCount; ++i) {
            // Validate individual frame size
            if (frameSizes[i] > MAX_FRAME_SIZE) {
                return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                    ServerError(ServerError::InvalidData, fmt::format("Frame {} size {} exceeds maximum {} bytes", i,
                                                                      frameSizes[i], MAX_FRAME_SIZE))};
            }

            // Track total data size to prevent memory exhaustion
            totalDataSize += frameSizes[i];
            if (totalDataSize > MAX_TOTAL_DATA_SIZE) {
                return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{ServerError(
                    ServerError::InvalidData, fmt::format("Total frame data size {} exceeds maximum {} bytes",
                                                          totalDataSize, MAX_TOTAL_DATA_SIZE))};
            }

            try {
                frames[i].resize(frameSizes[i]);
            } catch (const std::bad_alloc &e) {
                return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
                    ServerError(ServerError::InternalError, fmt::format("Failed to allocate {} bytes for frame {}: {}",
                                                                        frameSizes[i], i, e.what()))};
            }

            file.read(reinterpret_cast<char *>(frames[i].data()), frameSizes[i]);
            if (!file.good() || static_cast<uint32_t>(file.gcount()) != frameSizes[i]) {
                return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{ServerError(
                    ServerError::InvalidData, fmt::format("Failed to read frame {} data: expected {} bytes, got {}", i,
                                                          frameSizes[i], file.gcount()))};
            }
        }

        return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
            std::make_pair(std::move(frames), sourceInfo)};

    } catch (const std::exception &e) {
        return Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>{
            ServerError(ServerError::InternalError, fmt::format("Exception loading cache file: {}", e.what()))};
    }
}

Result<void> AudioCache::saveAsOggOpusWithMetadata(const std::string &oggFilePath,
                                                   const std::vector<std::vector<uint8_t>> &frames,
                                                   const SourceFileInfo &sourceInfo) const {

    try {
        std::ofstream file(oggFilePath, std::ios::binary);
        if (!file.is_open()) {
            return Result<void>(
                ServerError(ServerError::Forbidden, fmt::format("Cannot create cache file: {}", oggFilePath)));
        }

        // Simple binary format for now
        // In production, you'd create proper OGG Opus files with embedded metadata

        // Write metadata (simplified JSON-like format)
        std::string metadata = fmt::format("{{\"path\":\"{}\",\"size\":{},\"checksum\":\"{}\"}}", sourceInfo.filePath,
                                           sourceInfo.fileSize, sourceInfo.checksum);

        uint32_t metadataSize = static_cast<uint32_t>(metadata.size());
        file.write(reinterpret_cast<const char *>(&metadataSize), sizeof(metadataSize));
        file.write(metadata.data(), metadata.size());

        // Write frame count
        uint32_t frameCount = static_cast<uint32_t>(frames.size());
        file.write(reinterpret_cast<const char *>(&frameCount), sizeof(frameCount));

        // Write frame sizes
        for (const auto &frame : frames) {
            uint32_t frameSize = static_cast<uint32_t>(frame.size());
            file.write(reinterpret_cast<const char *>(&frameSize), sizeof(frameSize));
        }

        // Write frame data
        for (const auto &frame : frames) {
            file.write(reinterpret_cast<const char *>(frame.data()), frame.size());
        }

        if (!file.good()) {
            return Result<void>(ServerError(ServerError::InternalError, "Failed to write cache file data"));
        }

        return Result<void>();

    } catch (const std::exception &e) {
        return Result<void>(
            ServerError(ServerError::InternalError, fmt::format("Exception saving cache file: {}", e.what())));
    }
}

Result<void> AudioCache::ensureCacheDirectoryWritable() const {
    try {
        // Create cache directory if it doesn't exist
        if (!std::filesystem::exists(cacheDirectory_)) {
            std::filesystem::create_directories(cacheDirectory_);
        }

        // Check if directory is writable by trying to create a test file
        auto testFilePath = std::filesystem::path(cacheDirectory_) / ".write_test";
        {
            std::ofstream testFile(testFilePath);
            if (!testFile.is_open()) {
                return Result<void>(ServerError(ServerError::Forbidden,
                                                fmt::format("Cache directory is not writable: {}", cacheDirectory_)));
            }
        }

        // Clean up test file
        std::filesystem::remove(testFilePath);

        return Result<void>();

    } catch (const std::exception &e) {
        return Result<void>(
            ServerError(ServerError::InternalError, fmt::format("Failed to validate cache directory: {}", e.what())));
    }
}

bool AudioCache::allCacheFilesExist(const std::string &sourceFilePath) const {
    for (uint8_t ch = 0; ch < RTP_STREAMING_CHANNELS; ++ch) {
        auto cachePath = getCacheFilePath(sourceFilePath, ch);
        if (!std::filesystem::exists(cachePath)) {
            return false;
        }
    }
    return true;
}
