#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace creatures::audio {

/// Resolve a sound file by **basename** within `root`.
///
/// Tries the top-level `root / filename` first (the common case, and a cheap way
/// to preserve historical flat-store behavior), then falls back to a recursive
/// walk of `root` so that sounds living in subdirectories — notably permanent
/// dialog renders under `dialog/` — resolve too (issue #46).
///
/// Returns the canonical absolute path of the first matching regular file that
/// resolves safely inside `root`, or `std::nullopt` if nothing matches (or the
/// root doesn't exist). `recursive_directory_iterator` does not follow symlinks
/// by default, and every candidate is re-checked with a canonical within-root
/// test, so a match cannot escape `root`.
///
/// `filename` must be a bare basename with no path separators. The resolver
/// enforces that contract even when a caller forgets to pre-validate it.
///
/// Prefer SoundStoreIndex for request-path lookups (issue #94); this walks the
/// tree on every miss and picks duplicates by iteration order, and remains only
/// for one-shot callers (dialog re-render recovery).
std::optional<std::string> resolveSoundInRoot(const std::filesystem::path &root, const std::string &filename);

/// Is `filename` a safe bare basename for the sound stores?
///
/// Enforces (issue #94): non-empty, at most 255 bytes, well-formed UTF-8, no
/// C0 (U+0000–U+001F), DEL, or C1 (U+0080–U+009F) control characters, no NUL,
/// and no path components (absolute paths, roots, `.`/`..`, separators).
bool isSafeSoundFilename(const std::string &filename);

/// Bounded, printable rendering of an untrusted filename for logs and span
/// attributes: control and non-ASCII bytes are escaped as \xNN and the result
/// is truncated (with an ellipsis marker) so a hostile name can't flood logs
/// or smuggle terminal control sequences (issue #94).
std::string sanitizeForLogging(const std::string &filename);

/**
 * Basename → canonical-path index over one sound store root (issue #94).
 *
 * Request threads used to pay a full recursive tree walk for every basename
 * miss, and duplicate basenames resolved by directory-iteration order. The
 * index walks the tree once per invalidation (single-flight: the first reader
 * after markDirty() rebuilds while others wait) and represents duplicates
 * explicitly so callers can return a deterministic ambiguity error.
 *
 * Out-of-band writers are a supported workflow (modernize-sounds.py rewrites
 * files in place; streaming speech sessions and CreatureVoicesLib write
 * without the storage facade), so the index is advisory with cheap
 * self-healing: every hit is stat-revalidated, vanished entries are dropped,
 * and a miss falls back to a single-file probe of `root/basename` — never a
 * tree walk.
 *
 * Entries also record a WAV's header facts (sample rate, channels, whether it
 * matches the 17-channel/48 kHz RTP contract) from one cheap header parse at
 * index time — the format/health metadata issue #55 wants play-time lookups
 * and the sound list to share.
 *
 * Thread-safe: many concurrent readers (HTTP threads), rare rebuilds. Never
 * used from the 1 ms event-loop thread.
 */
class SoundStoreIndex {
  public:
    enum class Status { Found, NotFound, Ambiguous };

    struct Entry {
        std::string canonicalPath;
        std::uintmax_t sizeBytes{0};
        std::filesystem::file_time_type lastWrite{};
        // WAV header facts (issue #55 tie-in); zero/false for non-WAV files or
        // unparseable headers.
        uint32_t sampleRate{0};
        uint16_t channels{0};
        bool rtpPlayable{false};
    };

    struct Lookup {
        Status status{Status::NotFound};
        std::optional<Entry> entry;
        // Root-RELATIVE paths of every candidate when Ambiguous, sorted so the
        // error is deterministic and safe to echo to clients (no server
        // filesystem layout in the message).
        std::vector<std::string> candidates;
    };

    explicit SoundStoreIndex(std::filesystem::path root);

    /// Look up a basename. Rebuilds first if the index is dirty (single-flight).
    [[nodiscard]] Lookup find(const std::string &basename);

    /// O(1), callable from any thread. The next find() pays for one rebuild.
    void markDirty() noexcept { dirty_.store(true, std::memory_order_relaxed); }

    /// Synchronous rebuild — for the debug endpoints and tests.
    void rebuildNow();

    /// Number of indexed files (post-rebuild if dirty). For diagnostics/tests.
    [[nodiscard]] std::size_t entryCount();

  private:
    void rebuildIfDirty();
    void rebuild();
    [[nodiscard]] static std::optional<Entry> makeEntry(const std::filesystem::path &canonicalRoot,
                                                        const std::filesystem::path &file);
    [[nodiscard]] Lookup probeSingleFile(const std::string &basename);

    const std::filesystem::path root_;
    std::atomic<bool> dirty_{true};
    // False until the first successful build has been swapped in: readers that
    // arrive while the initial walk is in flight must wait for it rather than
    // read an empty map (subsequent rebuilds serve the previous coherent map).
    std::atomic<bool> built_{false};
    std::mutex rebuildMutex_; // single-flight for rebuilds
    mutable std::shared_mutex mapMutex_;
    // Bumped (under mapMutex_) every time rebuild() swaps the map, so a
    // self-heal write-back computed from a pre-swap snapshot is discarded
    // instead of clobbering the fresh entries.
    std::uint64_t rebuildGeneration_{0};
    std::unordered_map<std::string, std::vector<Entry>> entries_;
};

} // namespace creatures::audio
