#pragma once

#include <memory>
#include <string>

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

} // namespace creatures::ws
