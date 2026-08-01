#pragma once

#include <cstddef>
#include <string>

namespace creatures::util {

/// Lowercase ASCII filename component made from human text. Unsupported
/// characters become separators; the fallback is returned when no characters
/// survive. This is the one slug policy used by generated sound filenames.
[[nodiscard]] std::string slugify(const std::string &value, std::size_t maxLength = 40,
                                  const std::string &fallback = "sound");

} // namespace creatures::util
