#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace creatures::voice {

/// One entry of a packed timed-token list: a token held over [start, end] seconds.
/// Used for both the iXML LIPSYNC mouth cues (token = Rhubarb shape letter) and the
/// WORD_ALIGNMENT words (token = the spoken word) — the two share this on-the-wire
/// format so writer and reader can never drift (issue #56 review).
struct TimedToken {
    double start; // seconds
    double end;   // seconds
    std::string token;
};

/// Pack a list into `"start end token;start end token;..."`, seconds to 3 decimals.
/// A `;` inside a token is replaced with `,` since `;` is the entry separator (mouth
/// shapes never contain one; word tokens are whitespace-delimited so they don't
/// either, but the guard keeps the format total). This is the exact inverse of
/// parseTimedTokens — they are the single source of truth for the format.
[[nodiscard]] std::string packTimedTokens(const std::vector<TimedToken> &tokens);

/// Parse the packed form produced by packTimedTokens. Malformed entries (missing the
/// two spaces separating start/end/token) are skipped with a warning rather than
/// silently dropped. The input is expected already XML-unescaped.
[[nodiscard]] std::vector<TimedToken> parseTimedTokens(std::string_view packed);

} // namespace creatures::voice
