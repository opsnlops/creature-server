//
// AudioCache.h - Fast Opus file caching system for creature audio
//
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
     * @brief How aggressively a prune pass reclaims space (issue #225).
     *
     * The unusable tiers — orphaned, incomplete, abandoned temporaries, stale
     * lock files — are ALWAYS reclaimed; nothing can read them, so there is
     * no policy to make. These two knobs govern the tiers that discard cache
     * entries which would still work, trading a future re-encode for disk:
     *
     *  - `maxIdleAge`: drop entries not read in longer than this.
     *  - `maxTotalBytes`: hold the surviving set under a ceiling, evicting
     *    least-recently-read first.
     *
     * An unset/zero value disables that tier, which is why the default
     * constructed policy is exactly the historical orphan-only behavior.
     *
     * Deleting a usable entry only costs a cache miss (a slow re-encode), the
     * same thing that happens the first time any sound plays — so the policy
     * is tuned to be conservative rather than tidy. See config.h for how the
     * production defaults are chosen and why travel mode differs.
     */
    struct PrunePolicy {
        /// Entries not read in longer than this are evicted. Unset = keep forever.
        std::optional<std::chrono::seconds> maxIdleAge{};

        /// Ceiling on the retained cache, in bytes. 0 = unbounded.
        std::uintmax_t maxTotalBytes{0};

        /// Checked between entries so a long sweep can be abandoned at
        /// shutdown instead of delaying it. Unset = run to completion.
        std::function<bool()> abortRequested{};

        /**
         * @brief The policy this host should run with.
         *
         * Reads AUDIO_CACHE_MAX_IDLE_DAYS / AUDIO_CACHE_MAX_BYTES, falling
         * back to the mode-appropriate defaults in config.h. Resolved ONCE at
         * startup rather than per sweep, so the policy in the logs is the
         * policy in effect.
         */
        static PrunePolicy forHost(bool travelMode);

        /// Human-readable summary, for the startup log line.
        std::string describe() const;
    };

    /**
     * @brief What a prune pass found and (unless dry-run) removed.
     */
    struct PruneReport {
        std::size_t entriesScanned{0};
        std::size_t orphanedEntries{0};   // source file no longer exists
        std::size_t incompleteEntries{0}; // no completion marker (crashed save)
        std::size_t temporaryFiles{0};    // abandoned *.tmp.<pid>.<n>
        std::size_t orphanedLockFiles{0}; // .lock whose cache directory is gone
        std::size_t idleEntries{0};       // usable, but unread past maxIdleAge
        std::size_t oversizeEntries{0};   // usable, evicted LRU to fit maxTotalBytes
        std::size_t skippedEntries{0};    // candidate that raced a read, or lock unavailable
        std::uintmax_t bytesReclaimed{0};
        std::uintmax_t bytesRetained{0}; // size of the cache the pass left behind
        bool dryRun{true};
        bool aborted{false}; // shutdown interrupted the sweep
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
     * @brief Prune under an explicit policy (issue #225).
     *
     * Does everything pruneOrphanedEntries() does, then — if the policy asks
     * for them — evicts entries that are still usable but idle, and finally
     * trims the survivors to a byte ceiling least-recently-read first.
     *
     * Safe to call concurrently with playback. Every deletion is taken under
     * this key's process-local mutex AND its sibling advisory lock file; an
     * entry whose lock cannot be acquired is skipped, never deleted, because
     * losing a race here means tearing audio out from under a live show. The
     * idle and size tiers sample last-use times before sorting, so each
     * deletion re-reads the entry's last-use marker under the lock and backs
     * out if the entry has been read since it was sampled.
     *
     * Must NOT be called from the 1 ms event loop thread: it walks the cache
     * directory and stats every channel file.
     */
    Result<PruneReport> prune(const PrunePolicy &policy, bool dryRun = true,
                              std::shared_ptr<OperationSpan> parentSpan = nullptr);

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

    /**
     * Record that an entry is being read, by touching the mtime of its
     * `.complete` marker (issue #225).
     *
     * There is no access time to lean on: `atime` is unusable on `noatime` /
     * `relatime` mounts, which is most of them. The marker already exists per
     * entry, so re-using its mtime as "last read" costs one utimensat per
     * cache hit and adds no new files. Entries written before this existed
     * simply start from their save time, which is a fine approximation.
     *
     * Called as soon as a read is ABOUT to begin, not when it finishes, so an
     * in-flight load already looks recently-used to a concurrent prune even
     * if the advisory lock were somehow bypassed.
     */
    void markEntryRead(const std::string &cacheDirectory) const;

    /// Last-read time of an entry, from its `.complete` marker. Falls back to
    /// the directory's own mtime if the marker cannot be statted.
    std::optional<std::filesystem::file_time_type> lastReadTime(const std::filesystem::path &cacheDirectory) const;

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
