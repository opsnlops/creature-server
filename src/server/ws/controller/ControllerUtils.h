#pragma once

#include <cctype>
#include <memory>
#include <string>
#include <string_view>

#include <oatpp/web/protocol/http/incoming/Request.hpp>

#include "util/ObservabilityManager.h"

namespace creatures::ws {

/// Extract the W3C traceparent header from an incoming oatpp request.
/// Returns an empty string when the header is absent.
inline std::string extractTraceparent(
    const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request) {
    if (!request) return "";
    auto tp = request->getHeader("traceparent");
    return tp ? std::string(tp) : "";
}

/// Populate common HTTP semantic-convention attributes on a RequestSpan.
inline void addHttpRequestAttributes(
    const std::shared_ptr<creatures::RequestSpan> &span,
    const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request) {
    if (!span || !request) return;

    span->setAttribute("http.method", std::string(request->getStartingLine().method.toString()));
    span->setAttribute("http.target", std::string(request->getStartingLine().path.toString()));

    if (auto userAgent = request->getHeader("User-Agent")) {
        span->setAttribute("http.user_agent", std::string(userAgent));
    }
    if (auto contentLength = request->getHeader("Content-Length")) {
        span->setAttribute("http.request_content_length", std::string(contentLength));
    }
    if (auto host = request->getHeader("Host")) {
        span->setAttribute("http.host", std::string(host));
    }
    span->setAttribute("http.flavor", "1.1");
}

/// Cheap RFC 4122 UUID shape check. Accepts the canonical 8-4-4-4-12 hex form,
/// case-insensitive. Doesn't validate version/variant bits — just shape. Used at
/// controller path-param boundaries to keep arbitrary attacker-controlled strings
/// out of log lines and span attributes (security review M4).
inline bool isUuidShape(std::string_view s) {
    if (s.size() != 36)
        return false;
    constexpr int dashPositions[] = {8, 13, 18, 23};
    for (int pos : dashPositions) {
        if (s[pos] != '-')
            return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23)
            continue;
        if (!std::isxdigit(static_cast<unsigned char>(s[i])))
            return false;
    }
    return true;
}

} // namespace creatures::ws
