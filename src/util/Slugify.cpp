#include "util/Slugify.h"

#include <algorithm>
#include <cctype>

namespace creatures::util {

std::string slugify(const std::string &value, std::size_t maxLength, const std::string &fallback) {
    std::string slug;
    slug.reserve(std::min(value.size(), maxLength));
    bool lastDash = false;
    for (const unsigned char character : value) {
        if (std::isalnum(character)) {
            slug.push_back(static_cast<char>(std::tolower(character)));
            lastDash = false;
        } else if (!slug.empty() && !lastDash) {
            slug.push_back('-');
            lastDash = true;
        }
        if (slug.size() >= maxLength) {
            break;
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    return slug.empty() ? fallback : slug;
}

std::string exportBasename(const std::string &title, const std::string &id, std::size_t maxLength,
                           const std::string &fallback, std::size_t idTail) {
    return slugify(title, maxLength, fallback) + "-" + id.substr(0, std::min(idTail, id.size()));
}

std::string titleExcerpt(const std::string &text, std::size_t maxLength, const std::string &fallback) {
    std::string collapsed;
    bool lastSpace = true; // also trims leading whitespace
    for (const char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!lastSpace) {
                collapsed.push_back(' ');
                lastSpace = true;
            }
        } else {
            collapsed.push_back(ch);
            lastSpace = false;
        }
    }
    while (!collapsed.empty() && collapsed.back() == ' ') {
        collapsed.pop_back();
    }
    if (collapsed.empty()) {
        return fallback;
    }
    if (collapsed.size() <= maxLength) {
        return collapsed;
    }
    auto cut = collapsed.rfind(' ', maxLength);
    if (cut == std::string::npos || cut == 0) {
        cut = maxLength;
        // Never split a UTF-8 sequence when forced to cut mid-word.
        while (cut > 0 && (static_cast<unsigned char>(collapsed[cut]) & 0xC0) == 0x80) {
            cut--;
        }
    }
    std::string excerpt = collapsed.substr(0, cut);
    // No dangling separators in front of the ellipsis.
    while (!excerpt.empty() && (excerpt.back() == ' ' || excerpt.back() == ',')) {
        excerpt.pop_back();
    }
    return excerpt + "…";
}

std::string bgmExportBasename(const std::string &scriptTitle, const std::string &prompt,
                              const std::string &generationId) {
    return slugify(scriptTitle, 48, "dialog") + "--bgm--" + slugify(prompt, 56, "music") + "--" +
           generationId.substr(0, std::min<std::size_t>(12, generationId.size()));
}

} // namespace creatures::util
