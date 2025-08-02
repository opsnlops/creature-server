//
// AudioCache.h - Fast Opus file caching system for creature audio
//
#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "server/config.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::util {

/**
 * @brief Fast cache for pre-encoded Opus audio files
 * 
 * This class provides a caching layer for 17-channel WAV files that have been
 * encoded to Opus. Cached files are stored as OGG Opus files with embedded
 * metadata for cache invalidation. The goal is to complete cache validation
 * in under 20ms (one frame).
 */
class AudioCache {
  public:
    /**
     * @brief Metadata about a source audio file for cache validation
     */
    struct SourceFileInfo {
        std::string filePath;
        std::filesystem::file_time_type modTime;
        std::uintmax_t fileSize;
        std::string checksum;  // SHA-256 hash of file content
        
        bool operator==(const SourceFileInfo& other) const {
            return filePath == other.filePath &&
                   modTime == other.modTime &&
                   fileSize == other.fileSize &&
                   checksum == other.checksum;
        }
    };
    
    /**
     * @brief Cached audio data for all 17 channels
     */
    struct CachedAudioData {
        std::size_t framesPerChannel;
        std::array<std::vector<std::vector<uint8_t>>, RTP_STREAMING_CHANNELS> encodedFrames;
    };

    AudioCache(const std::string& soundDirectory);
    ~AudioCache() = default;

    /**
     * @brief Try to load cached audio data for a source file
     * 
     * Fast path: checks if cached OGG files exist and are valid.
     * Should complete in <20ms for cache hits.
     * 
     * @param sourceFilePath Path to source WAV file
     * @param parentSpan Optional telemetry span
     * @return Cached audio data if valid cache exists, nullptr otherwise
     */
    std::shared_ptr<CachedAudioData> tryLoadFromCache(
        const std::string& sourceFilePath,
        std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * @brief Save encoded audio data to cache
     * 
     * Stores the encoded frames as OGG Opus files (one per channel) with
     * embedded metadata about the source file for cache validation.
     * 
     * @param sourceFilePath Path to source WAV file  
     * @param audioData Encoded audio data to cache
     * @param parentSpan Optional telemetry span
     * @return Result indicating success/failure
     */
    Result<void> saveToCache(
        const std::string& sourceFilePath,
        const CachedAudioData& audioData,
        std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * @brief Clear all cached files for a source file
     * 
     * @param sourceFilePath Path to source WAV file
     * @return Result indicating success/failure  
     */
    Result<void> clearCache(const std::string& sourceFilePath);

    /**
     * @brief Get cache statistics
     */
    struct CacheStats {
        std::size_t totalCachedFiles;
        std::size_t totalDiskUsage;
        std::size_t cacheHits;
        std::size_t cacheMisses;
    };
    CacheStats getStats() const;

  private:
    std::string soundDirectory_;
    std::string cacheDirectory_;
    
    // Cache statistics
    mutable std::size_t cacheHits_ = 0;
    mutable std::size_t cacheMisses_ = 0;

    /**
     * @brief Generate cache file path for a source file and channel
     */
    std::string getCacheFilePath(const std::string& sourceFilePath, uint8_t channel) const;
    
    /**
     * @brief Generate cache directory path for a source file
     */
    std::string getCacheDirectoryPath(const std::string& sourceFilePath) const;
    
    /**
     * @brief Extract source file information for cache validation
     */
    Result<SourceFileInfo> getSourceFileInfo(const std::string& filePath) const;
    
    /**
     * @brief Calculate SHA-256 checksum of a file (fast, streaming)
     */
    Result<std::string> calculateFileChecksum(const std::string& filePath) const;
    
    /**
     * @brief Load OGG Opus file and extract embedded metadata
     */
    Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>> 
    loadOggOpusWithMetadata(const std::string& oggFilePath) const;
    
    /**
     * @brief Save audio frames as OGG Opus with embedded metadata
     */
    Result<void> saveAsOggOpusWithMetadata(
        const std::string& oggFilePath,
        const std::vector<std::vector<uint8_t>>& frames,
        const SourceFileInfo& sourceInfo) const;
    
    /**
     * @brief Ensure cache directory exists and is writable
     */
    Result<void> ensureCacheDirectoryWritable() const;
    
    /**
     * @brief Check if all cache files exist for a source file
     */
    bool allCacheFilesExist(const std::string& sourceFilePath) const;
};

} // namespace creatures::util