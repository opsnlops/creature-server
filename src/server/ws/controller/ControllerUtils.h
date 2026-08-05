#pragma once

#include <cctype>
#include <memory>
#include <string>
#include <string_view>

#include <oatpp/core/data/stream/BufferStream.hpp>
#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/web/protocol/http/incoming/Request.hpp>

#include "server/metrics/counters.h"
#include "util/ObservabilityManager.h"
#include "util/helpers.h"

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
/// Consume an entity body the client sent on a method whose handlers never
/// read one (issue #122).
///
/// oatpp only reads the request body when an endpoint declares one via
/// BODY_STRING/BODY_DTO. None of our GET or DELETE endpoints do — there's
/// nothing legitimate to read — so if a client sends a body anyway, those
/// bytes stay in the socket. On a keep-alive connection the NEXT request is
/// then parsed starting at the leftovers, and its method token arrives with
/// the previous body glued to the front:
///
///     "No mapping for HTTP-method: '{}DELETE', URL: '/api/v1/stage/...'"
///
/// That's HTTP framing corruption, not a routing failure, and it's in the
/// request-smuggling family — the next request's shape is influenced by the
/// previous request's body. It presents as an intermittent 404 because it
/// depends on landing on a pooled connection.
///
/// Draining here rather than adding BODY_STRING to every GET and DELETE keeps
/// it impossible to forget on a new endpoint. It is deliberately scoped to
/// methods we know never declare a body: draining one oatpp has already read
/// would block waiting for bytes the peer isn't going to send.
template <typename SpanT>
inline void drainUnreadRequestBody(const std::string &method,
                                   const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request,
                                   const SpanT &span) {
    if (method != "GET" && method != "DELETE") {
        return; // POST/PUT/PATCH declare their bodies; oatpp has read them already.
    }
    if (!request) {
        return;
    }

    // Only touch the stream when the client actually framed a body. Reading
    // when there is nothing to read risks blocking on the socket.
    const auto contentLength = request->getHeader("Content-Length");
    const bool hasContentLength = contentLength && contentLength->size() > 0 && contentLength != "0";
    const auto transferEncoding = request->getHeader("Transfer-Encoding");
    const bool isChunked = transferEncoding && transferEncoding->size() > 0;
    if (!hasContentLength && !isChunked) {
        return;
    }

    try {
        oatpp::data::stream::BufferOutputStream sink;
        request->transferBodyToStream(&sink);
        if (span) {
            span->setAttribute("http.request.body_drained", static_cast<int64_t>(sink.getCurrentPosition()));
        }
    } catch (const std::exception &e) {
        // A failure here means the connection is already in a bad state; the
        // response still goes out and oatpp will tear the connection down.
        warn("failed to drain unread {} request body: {}", method, e.what());
    }
}

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
    drainUnreadRequestBody(method, request, span);
    return withSpanStatus(span, [&] { return work(span); });
}

// isUuidShape lives in util/helpers.h so non-controller callers (JobWorker,
// model parsers) can share the single canonical check. We re-export it into
// the ws namespace so existing controller call sites stay unqualified.
using creatures::isUuidShape;

} // namespace creatures::ws
