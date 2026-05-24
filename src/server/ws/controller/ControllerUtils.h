#pragma once

#include <cctype>
#include <memory>
#include <string>
#include <string_view>

#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/web/protocol/http/incoming/Request.hpp>

#include "server/metrics/counters.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<SystemCounters> metrics;
} // namespace creatures

namespace creatures::ws {

/// Extract the W3C traceparent header from an incoming oatpp request.
/// Returns an empty string when the header is absent.
inline std::string extractTraceparent(const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request) {
    if (!request)
        return "";
    auto tp = request->getHeader("traceparent");
    return tp ? std::string(tp) : "";
}

/// Populate common HTTP semantic-convention attributes on a RequestSpan.
inline void addHttpRequestAttributes(const std::shared_ptr<creatures::RequestSpan> &span,
                                     const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request) {
    if (!span || !request)
        return;

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
    // Renamed from http.flavor — that column was inferred as boolean in Honeycomb
    // by an early write, so every subsequent string ("1.1") landed as `false`. Using
    // the modern OTel name http.protocol_version (which has no historical type
    // collision) keeps the value queryable as a string.
    span->setAttribute("http.protocol_version", "1.1");
}

/// Wrap an endpoint's work in a try/catch that updates the span's http.status_code on
/// exception paths and records the exception, then re-throws so oatpp's normal error
/// rendering still runs. Without this wrapper, the typical pattern of
/// `setHttpStatus(200)` on the success line means error paths (via OATPP_ASSERT_HTTP
/// or anything that throws) leave the span's status unset — so a Honeycomb query like
/// `WHERE http.status_code >= 400` misses real failures.
template <typename F>
auto withSpanStatus(const std::shared_ptr<creatures::RequestSpan> &span, F &&work) -> decltype(work()) {
    try {
        return work();
    } catch (oatpp::web::protocol::http::HttpError &e) {
        if (span) {
            span->setHttpStatus(e.getInfo().status.code);
            span->recordException(e);
        }
        throw;
    } catch (const std::exception &e) {
        if (span) {
            span->setHttpStatus(500);
            span->recordException(e);
        }
        throw;
    }
}

/// One-stop helper that combines the boilerplate every REST endpoint needs:
/// create the RequestSpan (with traceparent propagation), add HTTP attributes,
/// stamp endpoint/controller name, bump the REST counter, and wrap the body
/// in withSpanStatus so error paths are correctly reflected on the span.
/// `work` receives the span (which may be nullptr) and returns the response.
template <typename F>
auto runEndpoint(const std::string &spanName, const std::string &method, const std::string &path,
                 const std::string &endpointName, const std::string &controllerName,
                 const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request, F &&work)
    -> decltype(work(std::declval<const std::shared_ptr<creatures::RequestSpan> &>())) {
    const auto span =
        creatures::observability
            ? creatures::observability->createRequestSpan(spanName, method, path, extractTraceparent(request))
            : nullptr;
    addHttpRequestAttributes(span, request);
    if (creatures::metrics) {
        creatures::metrics->incrementRestRequestsProcessed();
    }
    if (span) {
        span->setAttribute("endpoint.name", endpointName);
        span->setAttribute("controller.name", controllerName);
    }
    return withSpanStatus(span, [&] { return work(span); });
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
