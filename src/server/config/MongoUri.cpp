#include "server/config/MongoUri.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>
#include <vector>

namespace creatures::mongo {
namespace {

struct DeadlineOption {
    std::string_view name;
    std::int32_t maximum;
};

constexpr std::array<DeadlineOption, 5> DEADLINE_OPTIONS{{
    {"serverSelectionTimeoutMS", SERVER_SELECTION_TIMEOUT_MS},
    {"connectTimeoutMS", CONNECT_TIMEOUT_MS},
    {"socketTimeoutMS", SOCKET_TIMEOUT_MS},
    {"waitQueueTimeoutMS", WAIT_QUEUE_TIMEOUT_MS},
    {"wTimeoutMS", WRITE_CONCERN_TIMEOUT_MS},
}};

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return result;
}

std::optional<std::int32_t> positiveInteger(std::string_view value) {
    std::int32_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed <= 0) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<std::size_t> optionIndex(std::string_view name) {
    const auto normalizedName = lowercase(name);
    for (std::size_t index = 0; index < DEADLINE_OPTIONS.size(); ++index) {
        if (lowercase(DEADLINE_OPTIONS[index].name) == normalizedName) {
            return index;
        }
    }
    return std::nullopt;
}

} // namespace

std::string normalizeUri(const std::string &uri) {
    const auto queryStart = uri.find('?');
    const auto base = uri.substr(0, queryStart);
    const auto query = queryStart == std::string::npos ? std::string{} : uri.substr(queryStart + 1);

    std::array<std::int32_t, DEADLINE_OPTIONS.size()> effectiveValues{};
    for (std::size_t index = 0; index < DEADLINE_OPTIONS.size(); ++index) {
        effectiveValues[index] = DEADLINE_OPTIONS[index].maximum;
    }

    std::vector<std::string> preservedOptions;
    std::size_t optionStart = 0;
    while (optionStart <= query.size() && !query.empty()) {
        const auto optionEnd = query.find('&', optionStart);
        const auto token =
            query.substr(optionStart, optionEnd == std::string::npos ? std::string::npos : optionEnd - optionStart);
        const auto equals = token.find('=');
        const auto name = token.substr(0, equals);
        const auto deadlineIndex = optionIndex(name);
        if (deadlineIndex) {
            const auto value =
                equals == std::string::npos ? std::string_view{} : std::string_view(token).substr(equals + 1);
            if (const auto parsed = positiveInteger(value)) {
                effectiveValues[*deadlineIndex] = std::min(effectiveValues[*deadlineIndex], *parsed);
            }
        } else if (!token.empty()) {
            preservedOptions.push_back(token);
        }

        if (optionEnd == std::string::npos) {
            break;
        }
        optionStart = optionEnd + 1;
    }

    std::string result = base;
    auto appendOption = [&result](const std::string &option) {
        result.push_back(result.find('?') == std::string::npos ? '?' : '&');
        result.append(option);
    };
    for (const auto &option : preservedOptions) {
        appendOption(option);
    }
    for (std::size_t index = 0; index < DEADLINE_OPTIONS.size(); ++index) {
        appendOption(std::string(DEADLINE_OPTIONS[index].name) + "=" + std::to_string(effectiveValues[index]));
    }
    return result;
}

std::string redactUri(const std::string &uri) {
    const auto schemeEnd = uri.find("://");
    if (schemeEnd == std::string::npos) {
        return "<invalid MongoDB URI>";
    }

    const auto authorityStart = schemeEnd + 3;
    const auto authorityEnd = uri.find_first_of("/?", authorityStart);
    const auto at = uri.rfind('@', authorityEnd == std::string::npos ? uri.size() : authorityEnd);
    if (at == std::string::npos || at < authorityStart) {
        return uri;
    }

    return uri.substr(0, authorityStart) + "<credentials>@" + uri.substr(at + 1);
}

} // namespace creatures::mongo
