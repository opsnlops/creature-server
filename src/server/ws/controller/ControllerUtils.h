#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include <oatpp/core/data/stream/BufferStream.hpp>
#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/web/protocol/http/incoming/Request.hpp>
#include <oatpp/web/protocol/http/outgoing/Body.hpp>
#include <oatpp/web/protocol/http/outgoing/Response.hpp>

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

// =============================================================================
// HEAD support for file-serving routes (#139)
//
// oatpp routes by exact method, so an unmapped HEAD falls through to the
// router's 404 — with the 404's Content-Length. A client sizing a download
// before committing to it was told a 28 MiB take was a 181-byte stub.
//
// RFC 9110: HEAD is GET without the body — same status, same headers. The
// only honest way to guarantee that is to run the GET and drop the bytes,
// which is what these two do. It costs the server the read (or the encode,
// for renditions) and saves the transfer, and Content-Length is right by
// construction rather than by a second code path that can drift.
// =============================================================================

/// A body that declares a Content-Length and then writes nothing.
///
/// oatpp overwrites Content-Length from `getKnownSize()` after the body
/// declares its headers, so a plain empty response can only ever say zero.
/// Reporting the size here and returning EOF from the read callback is what
/// lets the header stay truthful while no payload goes out.
class HeadBody : public oatpp::web::protocol::http::outgoing::Body {
  public:
    explicit HeadBody(v_int64 size) : size_(size) {}

    oatpp::v_io_size read(void *, v_buff_size, oatpp::async::Action &) override {
        return 0; // EOF straight away — a HEAD response carries no body
    }
    void declareHeaders(oatpp::web::protocol::http::Headers &) override {}
    p_char8 getKnownData() override { return nullptr; }
    v_int64 getKnownSize() override { return size_; }

  private:
    v_int64 size_;
};

/// Streams a file to the socket in chunks while still declaring an exact
/// Content-Length (#140).
///
/// The routes this replaces read the whole file into a `std::vector` and then
/// let `oatpp::String` copy it again — two full-size allocations per request,
/// so one 300 MB scene cost ~600 MB of RSS on a box that is also running the
/// show's event loop.
///
/// Deliberately NOT oatpp's `StreamingBody`: its `getKnownSize()` returns -1,
/// which switches the response to `Transfer-Encoding: chunked` and drops
/// Content-Length entirely — silently undoing #139, since HEAD's whole job is
/// to report that length. A known size keeps the response fixed-length and
/// keeps HEAD honest.
///
/// The file handle is opened by the handler and held here for the life of the
/// response. That's what makes this safe against a concurrent rewrite: the
/// storage facade writes via `.tmp` + rename, so a replacement gets a new
/// inode and this fd keeps reading the bytes whose length we already promised.
class FileBody : public oatpp::web::protocol::http::outgoing::Body {
  public:
    FileBody(const std::string &path, v_int64 size) : size_(size), remaining_(size) {
        // Set the buffer before open() so reads come from a decent-sized
        // window; the transfer loop hands us the header buffer, which is small.
        stream_.rdbuf()->pubsetbuf(ioBuffer_.data(), static_cast<std::streamsize>(ioBuffer_.size()));
        stream_.open(path, std::ios::binary);
    }

    [[nodiscard]] bool isOpen() const { return stream_.is_open(); }

    oatpp::v_io_size read(void *buffer, v_buff_size count, oatpp::async::Action &) override {
        if (remaining_ <= 0) {
            return 0;
        }
        const auto want = static_cast<std::streamsize>(std::min<v_int64>(static_cast<v_int64>(count), remaining_));
        stream_.read(static_cast<char *>(buffer), want);
        const auto got = stream_.gcount();
        if (got <= 0) {
            // Short read against a length we already sent. Framing is now
            // wrong for this connection, so say so loudly rather than let it
            // look like a mysterious client-side truncation (#122's family).
            warn("file body ended {} bytes early — the response is short of its declared Content-Length", remaining_);
            remaining_ = 0;
            return 0;
        }
        remaining_ -= got;
        return static_cast<oatpp::v_io_size>(got);
    }

    void declareHeaders(oatpp::web::protocol::http::Headers &) override {}
    p_char8 getKnownData() override { return nullptr; }
    v_int64 getKnownSize() override { return size_; }

  private:
    std::array<char, 65536> ioBuffer_{}; // declared first: the stream points at it
    std::ifstream stream_;
    v_int64 size_;
    v_int64 remaining_;
};

/// Convert a fully-formed GET response into its HEAD equivalent: same status,
/// same headers, same Content-Length, no body. Errors convert too — a 404 for
/// a missing file stays a 404 with the error's own length.
inline std::shared_ptr<oatpp::web::protocol::http::outgoing::Response>
asHeadResponse(const std::shared_ptr<oatpp::web::protocol::http::outgoing::Response> &full) {
    if (!full) {
        return full;
    }
    const auto body = full->getBody();
    const v_int64 size = body ? body->getKnownSize() : 0;
    auto head = oatpp::web::protocol::http::outgoing::Response::createShared(
        full->getStatus(), std::make_shared<HeadBody>(size < 0 ? 0 : size));
    for (const auto &header : full->getHeaders().getAll()) {
        head->putHeader(header.first.toString(), header.second.toString());
    }
    // Some headers aren't on the response yet — a body declares its own at send
    // time, which is where a DTO response's Content-Type comes from. Ask the
    // real body what it would have added, so HEAD doesn't quietly drop it.
    // Content-Length is deliberately not taken from here: Response::send
    // recomputes it from the body it actually has, which is ours.
    if (body) {
        oatpp::web::protocol::http::Headers declared;
        body->declareHeaders(declared);
        for (const auto &header : declared.getAll()) {
            head->putHeaderIfNotExists(header.first.toString(), header.second.toString());
        }
    }
    return head;
}

// isUuidShape lives in util/helpers.h so non-controller callers (JobWorker,
// model parsers) can share the single canonical check. We re-export it into
// the ws namespace so existing controller call sites stay unqualified.
using creatures::isUuidShape;

} // namespace creatures::ws
