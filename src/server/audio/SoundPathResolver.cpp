#include "server/audio/SoundPathResolver.h"

#include <algorithm>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "server/audio/MonoWavDownmixer.h"
#include "server/config.h"

namespace fs = std::filesystem;

namespace creatures::audio {

namespace {

// True if `canonicalFile` lies inside `canonicalRoot`. Both must already be
// canonical (symlinks resolved). Guards against a resolved path escaping the
// bucket root — the real security boundary for serving files.
bool isInsideRoot(const fs::path &canonicalRoot, const fs::path &canonicalFile) {
    const auto root = canonicalRoot.string();
    const auto file = canonicalFile.string();
    if (file.size() < root.size() || file.compare(0, root.size(), root) != 0) {
        return false;
    }
    // Exact match (the root itself) or a genuine child (next char is a separator).
    return file.size() == root.size() || file[root.size()] == static_cast<char>(fs::path::preferred_separator);
}

// Walk `root` recursively for the first regular file whose basename matches
// `filename`, returning its canonical path if it resolves safely inside `root`.
std::optional<std::string> findByBasename(const fs::path &canonicalRoot, const std::string &filename) {
    std::error_code ec;
    try {
        for (const auto &entry : fs::recursive_directory_iterator(canonicalRoot)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (entry.path().filename() != filename) {
                continue;
            }
            const auto canonicalFile = fs::canonical(entry.path(), ec);
            if (ec || !isInsideRoot(canonicalRoot, canonicalFile)) {
                continue;
            }
            return canonicalFile.string();
        }
    } catch (const std::exception &) {
        // A transient filesystem error mid-walk is treated as "not found" rather
        // than a hard failure; the caller surfaces a 404.
        return std::nullopt;
    }
    return std::nullopt;
}

// Shared basename contract: bare filename, no path components.
bool isBareBasename(const std::string &filename) {
    const fs::path requested(filename);
    return !filename.empty() && !requested.is_absolute() && !requested.has_root_path() &&
           requested == requested.filename();
}

// Directories the index never descends into: any dot-directory — which covers
// the Opus packet cache (`.opus_cache/<hostname>/...`) that AudioCache keeps
// inside the permanent sound root.
bool isSkippedDirectory(const fs::path &directory) {
    const auto name = directory.filename().string();
    return !name.empty() && name.front() == '.';
}

} // namespace

std::optional<std::string> resolveSoundInRoot(const fs::path &root, const std::string &filename) {
    if (!isBareBasename(filename)) {
        return std::nullopt;
    }

    std::error_code ec;
    if (!fs::exists(root, ec) || ec) {
        return std::nullopt;
    }
    const auto canonicalRoot = fs::canonical(root, ec);
    if (ec) {
        return std::nullopt;
    }

    // Fast path: a top-level file resolves directly without walking the tree.
    const auto flat = fs::canonical(canonicalRoot / filename, ec);
    if (!ec && fs::is_regular_file(flat, ec) && isInsideRoot(canonicalRoot, flat)) {
        return flat.string();
    }

    // Recursive fallback: dialog/ renders and any other subdir'd sound (#46).
    return findByBasename(canonicalRoot, filename);
}

bool isSafeSoundFilename(const std::string &filename) {
    constexpr std::size_t MAX_FILENAME_LENGTH = 255;
    if (filename.empty() || filename.size() > MAX_FILENAME_LENGTH) {
        return false;
    }

    // Decode as UTF-8 and reject control characters at the CODEPOINT level:
    // C0 (U+0000–U+001F), DEL, and C1 (U+0080–U+009F). A byte-level 0x80–0x9F
    // check would reject legitimate non-ASCII names, whose UTF-8 continuation
    // bytes live in that range; malformed UTF-8 is rejected outright instead
    // (issue #94).
    for (std::size_t i = 0; i < filename.size();) {
        const auto lead = static_cast<unsigned char>(filename[i]);
        std::size_t continuationBytes = 0;
        uint32_t codepoint = 0;
        uint32_t minimumCodepoint = 0;
        if (lead < 0x80) {
            codepoint = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            continuationBytes = 1;
            codepoint = lead & 0x1FU;
            minimumCodepoint = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            continuationBytes = 2;
            codepoint = lead & 0x0FU;
            minimumCodepoint = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            continuationBytes = 3;
            codepoint = lead & 0x07U;
            minimumCodepoint = 0x10000;
        } else {
            return false; // lone continuation byte or invalid lead
        }
        if (i + continuationBytes >= filename.size()) {
            return false; // truncated sequence
        }
        for (std::size_t offset = 1; offset <= continuationBytes; ++offset) {
            const auto continuation = static_cast<unsigned char>(filename[i + offset]);
            if ((continuation & 0xC0) != 0x80) {
                return false; // malformed continuation
            }
            codepoint = (codepoint << 6) | (continuation & 0x3FU);
        }
        if (codepoint < minimumCodepoint || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return false; // overlong encoding, out of range, or surrogate
        }
        if (codepoint < 0x20 || codepoint == 0x7F || (codepoint >= 0x80 && codepoint <= 0x9F)) {
            return false;
        }
        i += continuationBytes + 1;
    }

    const fs::path candidate(filename);

    // Reject absolute paths, root references, or anything with parent components
    if (candidate.is_absolute() || candidate.has_root_path() || candidate != candidate.filename()) {
        return false;
    }

    for (const auto &part : candidate) {
        if (part == ".." || part == "." || part.native().find('\0') != std::string::npos) {
            return false;
        }
    }

    return true;
}

std::string sanitizeForLogging(const std::string &filename) {
    constexpr std::size_t MAX_LOGGED_LENGTH = 64;
    std::string sanitized;
    sanitized.reserve(std::min(filename.size(), MAX_LOGGED_LENGTH) + 8);
    for (const char raw : filename) {
        if (sanitized.size() >= MAX_LOGGED_LENGTH) {
            sanitized += "…";
            break;
        }
        const auto byte = static_cast<unsigned char>(raw);
        if (byte >= 0x20 && byte < 0x7F) {
            sanitized += raw;
        } else {
            sanitized += fmt::format("\\x{:02x}", byte);
        }
    }
    return sanitized;
}

SoundStoreIndex::SoundStoreIndex(fs::path root) : root_(std::move(root)) {}

SoundStoreIndex::Lookup SoundStoreIndex::find(const std::string &basename) {
    if (!isBareBasename(basename)) {
        return {};
    }

    rebuildIfDirty();

    std::vector<Entry> matches;
    std::uint64_t snapshotGeneration = 0;
    {
        std::shared_lock lock(mapMutex_);
        snapshotGeneration = rebuildGeneration_;
        if (auto it = entries_.find(basename); it != entries_.end()) {
            matches = it->second;
        }
    }

    if (matches.empty()) {
        // Out-of-band writers are supported: a brand-new top-level file is
        // found by one single-file probe — never a tree walk (issue #94).
        return probeSingleFile(basename);
    }

    // Self-heal: stat-revalidate every candidate; a vanished or changed file
    // must not be served from a stale entry (modernize-sounds rewrites files
    // in place, TTL sweeps delete ad-hoc sessions). All stats use error_code
    // overloads — a transient filesystem error must degrade to not-found, not
    // throw a 500 out of a request thread.
    std::vector<Entry> live;
    bool changed = false;
    std::error_code canonicalError;
    const auto canonicalRoot = fs::canonical(root_, canonicalError);
    for (auto &entry : matches) {
        std::error_code ec;
        const auto size = fs::file_size(entry.canonicalPath, ec);
        if (ec || !fs::is_regular_file(entry.canonicalPath, ec) || ec) {
            changed = true;
            continue;
        }
        const auto mtime = fs::last_write_time(entry.canonicalPath, ec);
        if (!ec && (size != entry.sizeBytes || mtime != entry.lastWrite)) {
            changed = true;
            // The file changed under us. Only a successful, still-inside-root
            // refresh may be served; anything else (canonical failure, a
            // symlink swap escaping the root) drops the entry — a stale path
            // must never bypass the containment boundary (issue #94 review).
            if (canonicalError) {
                continue;
            }
            auto refreshed = makeEntry(canonicalRoot, entry.canonicalPath);
            if (!refreshed) {
                continue;
            }
            entry = *refreshed;
        }
        live.push_back(entry);
    }

    if (changed) {
        // Write back only if no rebuild swapped the map since our snapshot —
        // otherwise this stale view would clobber the fresh entries (issue #94
        // review).
        std::unique_lock lock(mapMutex_);
        if (rebuildGeneration_ == snapshotGeneration) {
            if (live.empty()) {
                entries_.erase(basename);
            } else {
                entries_[basename] = live;
            }
        }
    }

    if (live.empty()) {
        return probeSingleFile(basename);
    }
    if (live.size() > 1) {
        // Deterministic tiebreak, preserving the store's documented behavior
        // (issue #46): a single top-level file wins over subdir'd files of the
        // same name. Only subdir-vs-subdir duplicates — where the old resolver
        // picked whichever the directory iterator met first — are ambiguous.
        if (!canonicalError) {
            const Entry *topLevel = nullptr;
            bool multipleTopLevel = false;
            for (const auto &entry : live) {
                if (fs::path(entry.canonicalPath).parent_path() == canonicalRoot) {
                    multipleTopLevel = topLevel != nullptr;
                    topLevel = &entry;
                }
            }
            if (topLevel != nullptr && !multipleTopLevel) {
                Lookup lookup;
                lookup.status = Status::Found;
                lookup.entry = *topLevel;
                return lookup;
            }
        }
        Lookup lookup;
        lookup.status = Status::Ambiguous;
        lookup.candidates.reserve(live.size());
        for (const auto &entry : live) {
            std::error_code relativeError;
            auto relative = fs::relative(entry.canonicalPath, canonicalRoot, relativeError);
            lookup.candidates.push_back(relativeError ? fs::path(entry.canonicalPath).filename().string()
                                                      : relative.string());
        }
        std::sort(lookup.candidates.begin(), lookup.candidates.end());
        return lookup;
    }

    Lookup lookup;
    lookup.status = Status::Found;
    lookup.entry = live.front();
    return lookup;
}

void SoundStoreIndex::rebuildNow() {
    std::lock_guard rebuildLock(rebuildMutex_);
    rebuild();
}

std::size_t SoundStoreIndex::entryCount() {
    rebuildIfDirty();
    std::shared_lock lock(mapMutex_);
    std::size_t count = 0;
    for (const auto &[basename, matches] : entries_) {
        count += matches.size();
    }
    return count;
}

void SoundStoreIndex::rebuildIfDirty() {
    // built_ makes startup coherent: readers arriving while the FIRST build is
    // in flight (dirty_ already cleared) block on the rebuild mutex instead of
    // reading an empty map and 404ing files that exist. Later rebuilds serve
    // the previous coherent map, which is fine.
    if (!dirty_.load(std::memory_order_acquire) && built_.load(std::memory_order_acquire)) {
        return;
    }
    // Single-flight: the first reader after markDirty() pays for one bounded
    // walk; concurrent readers wait here instead of walking themselves.
    std::lock_guard rebuildLock(rebuildMutex_);
    if (dirty_.load(std::memory_order_acquire) || !built_.load(std::memory_order_acquire)) {
        rebuild();
    }
}

void SoundStoreIndex::rebuild() {
    // Clear the flag first: an invalidation racing this walk re-marks dirty
    // and the *next* find() rebuilds again, rather than us losing the signal.
    dirty_.store(false, std::memory_order_relaxed);

    std::unordered_map<std::string, std::vector<Entry>> fresh;

    std::error_code ec;
    const auto canonicalRoot = fs::canonical(root_, ec);
    if (ec) {
        // Root missing (e.g. no ad-hoc sounds yet): an empty index is correct;
        // the single-file probe still answers if the root appears later.
        {
            std::unique_lock lock(mapMutex_);
            entries_.clear();
            ++rebuildGeneration_;
        }
        built_.store(true, std::memory_order_release);
        return;
    }

    try {
        // skip_permission_denied: one unreadable subdirectory must not abort
        // the walk (or, worse, put the index into a rebuild-per-request loop).
        auto iterator = fs::recursive_directory_iterator(canonicalRoot, fs::directory_options::skip_permission_denied);
        const auto end = fs::end(iterator);
        for (auto it = fs::begin(iterator); it != end; ++it) {
            std::error_code entryError;
            if (it->is_directory(entryError) && isSkippedDirectory(it->path())) {
                it.disable_recursion_pending();
                continue;
            }
            if (entryError || !it->is_regular_file(entryError) || entryError) {
                continue;
            }
            if (auto entry = makeEntry(canonicalRoot, it->path())) {
                fresh[it->path().filename().string()].push_back(std::move(*entry));
            }
        }
    } catch (const std::exception &exception) {
        // Keep whatever was collected and DON'T re-mark dirty: doing so put
        // every request into a serialized tree walk until the filesystem
        // recovered (issue #94 review). The self-healing probe covers gaps and
        // the next invalidation retries the walk.
        spdlog::warn("SoundStoreIndex: rebuild of {} stopped early: {}", root_.string(), exception.what());
    }

    std::size_t files = 0;
    for (const auto &[basename, matches] : fresh) {
        files += matches.size();
    }
    spdlog::debug("SoundStoreIndex: indexed {} file(s) under {}", files, root_.string());

    {
        std::unique_lock lock(mapMutex_);
        entries_.swap(fresh);
        ++rebuildGeneration_;
    }
    built_.store(true, std::memory_order_release);
}

std::optional<SoundStoreIndex::Entry> SoundStoreIndex::makeEntry(const fs::path &canonicalRoot, const fs::path &file) {
    std::error_code ec;
    const auto canonicalFile = fs::canonical(file, ec);
    if (ec || !isInsideRoot(canonicalRoot, canonicalFile)) {
        return std::nullopt;
    }
    Entry entry;
    entry.canonicalPath = canonicalFile.string();
    entry.sizeBytes = fs::file_size(canonicalFile, ec);
    if (ec) {
        return std::nullopt;
    }
    entry.lastWrite = fs::last_write_time(canonicalFile, ec);
    if (ec) {
        return std::nullopt;
    }

    // One cheap header parse records the format facts play-time lookups and
    // the sound list can share (issue #55). Failures just leave the fields
    // zeroed — an unreadable header is itself useful health information.
    auto extension = canonicalFile.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (extension == ".wav") {
        if (auto stream = MonoWavStream::open(entry.canonicalPath); stream.isSuccess()) {
            const auto &wav = stream.getValue().value();
            entry.sampleRate = static_cast<uint32_t>(wav->sampleRate());
            entry.channels = wav->channels();
            entry.rtpPlayable =
                wav->sampleRate() == RTP_SRATE && wav->channels() == static_cast<uint16_t>(RTP_STREAMING_CHANNELS);
        }
    }
    return entry;
}

SoundStoreIndex::Lookup SoundStoreIndex::probeSingleFile(const std::string &basename) {
    std::error_code ec;
    const auto canonicalRoot = fs::canonical(root_, ec);
    if (ec) {
        return {};
    }
    const auto flat = fs::canonical(canonicalRoot / basename, ec);
    if (ec || !fs::is_regular_file(flat, ec) || !isInsideRoot(canonicalRoot, flat)) {
        return {};
    }
    auto entry = makeEntry(canonicalRoot, flat);
    if (!entry) {
        return {};
    }
    {
        std::unique_lock lock(mapMutex_);
        entries_[basename] = {*entry};
    }
    Lookup lookup;
    lookup.status = Status::Found;
    lookup.entry = std::move(*entry);
    return lookup;
}

} // namespace creatures::audio
