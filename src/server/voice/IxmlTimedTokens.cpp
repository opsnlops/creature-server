#include "server/voice/IxmlTimedTokens.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "server/namespace-stuffs.h"

namespace creatures::voice {

std::string packTimedTokens(const std::vector<TimedToken> &tokens) {
    std::string packed;
    packed.reserve(tokens.size() * 20);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) {
            packed += ';';
        }
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.3f %.3f ", tokens[i].start, tokens[i].end);
        packed += buf;
        // Guard the entry separator so a stray ';' can't corrupt the packing.
        std::string safe = tokens[i].token;
        std::replace(safe.begin(), safe.end(), ';', ',');
        packed += safe;
    }
    return packed;
}

std::vector<TimedToken> parseTimedTokens(std::string_view packed) {
    std::vector<TimedToken> out;
    std::size_t i = 0;
    while (i < packed.size()) {
        const auto semi = packed.find(';', i);
        const auto entry = packed.substr(i, semi == std::string_view::npos ? std::string_view::npos : semi - i);
        i = (semi == std::string_view::npos) ? packed.size() : semi + 1;

        const auto sp1 = entry.find(' ');
        const auto sp2 = (sp1 == std::string_view::npos) ? std::string_view::npos : entry.find(' ', sp1 + 1);
        if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
            warn("parseTimedTokens: skipping malformed entry '{}'", std::string(entry));
            continue;
        }

        // strtod stops at the space, so parse in place from the entry's char data
        // without allocating substrings for the two numbers.
        TimedToken t;
        t.start = std::strtod(entry.data(), nullptr);
        t.end = std::strtod(entry.data() + sp1 + 1, nullptr);
        t.token = std::string(entry.substr(sp2 + 1));
        out.push_back(std::move(t));
    }
    return out;
}

} // namespace creatures::voice
