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

} // namespace creatures::util
