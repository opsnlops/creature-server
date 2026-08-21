#pragma once

#include <cstddef>
#include <string>

namespace creatures::util {

/// Lowercase ASCII filename component made from human text. Unsupported
/// characters become separators; the fallback is returned when no characters
/// survive. This is the one slug policy used by generated sound filenames.
[[nodiscard]] std::string slugify(const std::string &value, std::size_t maxLength = 40,
                                  const std::string &fallback = "sound");

/// The one export-name shape (#126, #152): "what it is" slug plus a short id
/// tail so identically-titled artifacts never collide. Returns
/// "{slugify(title, maxLength, fallback)}-{first idTail chars of id}", no
/// extension — callers append their own. Every generated sound artifact
/// (dialog renders, voice takes, exchange downloads) names itself this way.
[[nodiscard]] std::string exportBasename(const std::string &title, const std::string &id, std::size_t maxLength = 48,
                                         const std::string &fallback = "dialog", std::size_t idTail = 8);

/// The accepted-BGM naming triple: "{title slug}--bgm--{prompt slug}--{id12}".
/// One definition keeps the permanent WAV (DialogMusicService) and the MP3
/// download name (DialogMusicController) describing the same identity.
[[nodiscard]] std::string bgmExportBasename(const std::string &scriptTitle, const std::string &prompt,
                                            const std::string &generationId);

} // namespace creatures::util
