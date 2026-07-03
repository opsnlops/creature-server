#pragma once

#include <filesystem>
#include <optional>
#include <string>

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
/// `filename` is expected to be a bare basename with no path separators; callers
/// are responsible for that sanitization (see SoundService's isSafeFilename).
/// Basename matching is safe because the only subdir'd files are dialog renders
/// named with globally-unique UUIDs — a collision with a top-level sound is not
/// possible in practice.
std::optional<std::string> resolveSoundInRoot(const std::filesystem::path &root, const std::string &filename);

} // namespace creatures::audio
