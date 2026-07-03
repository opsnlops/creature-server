#include "server/audio/SoundPathResolver.h"

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

} // namespace

std::optional<std::string> resolveSoundInRoot(const fs::path &root, const std::string &filename) {
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

} // namespace creatures::audio
