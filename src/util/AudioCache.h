//
// AudioCache.h - Fast Opus file caching system for creature audio
//
#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "server/config.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::util {

/**
 * @brief Fast cache for pre-encoded Opus audio files
 *
 * This class provides a caching layer for 17-channel WAV files that have been
 * encoded to Opus. Cached files use a versioned internal packet format with
 * source metadata for cache invalidation.
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
        std::string checksum; // SHA-256 hash of file content

        bool operator==(const SourceFileInfo &other) const {
            return filePath == other.filePath && modTime == other.modTime && fileSize == other.fileSize &&
                   checksum == other.checksum;
        }
    };

    /**
     * @brief Cached audio data for all 17 channels
     */
    struct CachedAudioData {
        std::size_t framesPerChannel;
        std::array<std::vector<std::vector<uint8_t>>, RTP_STREAMING_CHANNELS> encodedFrames;
        // Payload + per-frame overhead, accumulated by the reader as it
        // budgets the load. Carried out so the buffer doesn't re-walk ~1.5M
        // frame vectors to recompute a number we already have (issue #93).
        std::size_t approximateBytes{0};
    };

    AudioCache(const std::string &soundDirectory);
    ~AudioCache() = default;

    /**
     * @brief Try to load cached audio data for a source file
     *
     * Checks if every channel packet file exists and matches the source.
     *
     * @param sourceFilePath Path to source WAV file
     * @param parentSpan Optional telemetry span
     * @return Cached audio data if valid cache exists, nullptr otherwise
     */
    std::shared_ptr<CachedAudioData> tryLoadFromCache(const std::string &sourceFilePath,
                                                      std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * @brief Save encoded audio, verifying the source still matches a
     *        fingerprint captured BEFORE the WAV was read (issue #93).
     *
     * This is the ONLY save path: a variant that fingerprints at save time
     * cannot detect a source replaced during the encode, so it was removed
     * rather than left as a footgun for the next caller.
     *
     * Closes the encode-time TOCTOU: if an external writer replaced the WAV
     * between the caller's fingerprint capture and this publication, the save
     * is refused (InvalidData, no completion marker) instead of labeling the
     * old packets with the new file's identity — which would then validate as
     * a cache hit forever. Takes the frames by reference so the caller doesn't
     * copy ~hundreds of MB into a temporary struct.
     */
    Result<void> saveToCache(const std::string &sourceFilePath, std::size_t framesPerChannel,
                             const std::array<std::vector<std::vector<uint8_t>>, RTP_STREAMING_CHANNELS> &encodedFrames,
                             const SourceFileInfo &expectedSource, std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * @brief Fingerprint a source file (stat + SHA-256).
     *
     * Public so encode callers can capture the fingerprint BEFORE opening the
     * WAV and hand it to the verifying saveToCache overload (issue #93).
     */
    Result<SourceFileInfo> getSourceFileInfo(const std::string &filePath) const;

    /**
     * @brief Clear all cached files for a source file
     *
     * @param sourceFilePath Path to source WAV file
     * @return Result indicating success/failure
     */
    Result<void> clearCache(const std::string &sourceFilePath);

    /**
     * @brief What a prune pass found and (unless dry-run) removed.
     */
    struct PruneReport {
        std::size_t entriesScanned{0};
        std::size_t orphanedEntries{0};   // source file no longer exists
        std::size_t incompleteEntries{0}; // no completion marker (crashed save)
        std::size_t temporaryFiles{0};    // abandoned *.tmp.<pid>.<n>
        std::size_t orphanedLockFiles{0}; // .lock whose cache directory is gone
        std::uintmax_t bytesReclaimed{0};
        bool dryRun{true};
        // Bounded sample of what was (or would be) removed, for the operator.
        std::vector<std::string> removed;
    };

    /**
     * @brief Remove cache entries that can never be used again (issue #166).
     *
     * The cache is keyed by a hash of the source file's canonical path, so
     * MOVING or deleting a sound orphans its entry permanently — nothing else
     * in the system reclaims it. This walks the hostname-scoped cache
     * directory, recovers each entry's recorded source path from the small
     * metadata header of its channel-0 file, and drops entries whose source is
     * gone, entries with no completion marker, abandoned temporaries, and lock
     * files left without a directory.
     *
     * Deliberately separate from cache invalidation: deleting files as a side
     * effect of "invalidate caches" would be surprising.
     *
     * @param dryRun when true (the default) nothing is deleted; the report
     *               describes what would have been.
     */
    Result<PruneReport> pruneOrphanedEntries(bool dryRun = true, std::shared_ptr<OperationSpan> parentSpan = nullptr);

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
    mutable std::atomic<std::size_t> cacheHits_{0};
    mutable std::atomic<std::size_t> cacheMisses_{0};

    using CacheKeyMutex = std::recursive_mutex;
    mutable std::mutex keyMutexMapMutex_;
    mutable std::unordered_map<std::string, std::weak_ptr<CacheKeyMutex>> keyMutexes_;

    /**
     * @brief Generate cache file path for a source file and channel
     */
    std::string getCacheFilePath(const std::string &sourceFilePath, uint8_t channel) const;

    /**
     * @brief Generate cache directory path for a source file
     */
    std::string getCacheDirectoryPath(const std::string &sourceFilePath) const;

    /**
     * Advisory lock file for one cache key. Deliberately a SIBLING of the
     * cache directory, not a file inside it: clearCache's remove_all would
     * otherwise delete the very inode it holds the lock on, after which a peer
     * process creates a fresh inode and both proceed (issue #93 review).
     */
    std::string getCacheLockPath(const std::string &sourceFilePath) const;

    /**
     * clearCache's body, for callers that ALREADY hold this key's advisory
     * lock. flock is per-fd, so a holder that called the public clearCache
     * would block forever waiting on itself; making that mistake unwritable is
     * why this split exists (issue #93 review).
     */
    Result<void> clearCacheLocked(const std::string &sourceFilePath);

    /// Read ONLY the metadata header of a cache channel file to recover the
    /// source path it was built from. Cheap: the header is a few hundred bytes
    /// at the front, so pruning never reads packet payloads (issue #166).
    Result<std::string> peekCachedSourcePath(const std::string &cacheChannelPath) const;

    /// Remove abandoned `*.tmp.<pid>.<n>` files for this key. Caller holds the
    /// exclusive lock. Unique temp names never self-clean the way the old
    /// deterministic name did, so a crash mid-save would leak them (issue #93).
    void sweepStaleTemporaries(const std::string &cacheDir) const;

    std::shared_ptr<CacheKeyMutex> getKeyMutex(const std::string &sourceFilePath) const;

    /// Same per-key mutex, addressed by the cache directory directly. Pruning
    /// works from the cache side and has no source path to derive it from.
    std::shared_ptr<CacheKeyMutex> getKeyMutexForCacheDir(const std::string &cacheDirectory) const;

    /**
     * @brief Calculate SHA-256 checksum of a file (fast, streaming)
     */
    Result<std::string> calculateFileChecksum(const std::string &filePath) const;

    /**
     * @brief Load a cached channel file and extract embedded metadata.
     *
     * `aggregateBytes` accumulates payload + per-frame overhead across the
     * channels of one cache load; the budget is checked against
     * RTP_MAX_ENCODED_TOTAL_BYTES BEFORE each allocation (issue #93) so a
     * corrupt or hostile cache can't balloon to 17 × the per-file limit.
     */
    Result<std::pair<std::vector<std::vector<uint8_t>>, SourceFileInfo>>
    loadOggOpusWithMetadata(const std::string &oggFilePath, std::size_t &aggregateBytes) const;

    /**
     * @brief Save audio frames as OGG Opus with embedded metadata
     */
    Result<void> saveAsOggOpusWithMetadata(const std::string &oggFilePath,
                                           const std::vector<std::vector<uint8_t>> &frames,
                                           const SourceFileInfo &sourceInfo) const;

    /**
     * @brief Ensure cache directory exists and is writable
     */
    Result<void> ensureCacheDirectoryWritable() const;

    /**
     * @brief Check if all cache files exist for a source file
     */
    bool allCacheFilesExist(const std::string &sourceFilePath) const;
};

} // namespace creatures::util
