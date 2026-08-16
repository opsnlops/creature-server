
#pragma once

#include <oatpp/web/protocol/http/incoming/SimpleBodyDecoder.hpp>
#include <oatpp/web/server/interceptor/RequestInterceptor.hpp>
#include <oatpp/web/server/interceptor/ResponseInterceptor.hpp>

namespace creatures ::ws {

/*
 * oatpp 1.3 never drains an unread request body before parsing the next request on a
 * keep-alive connection. An endpoint declared without a BODY_* macro leaves any body the
 * client sent sitting in the connection's buffered stream, so the next request on that
 * pooled connection parses as garbage (e.g. method "{}GET") and fails routing. See
 * issue #142.
 *
 * The three pieces below close that hole; AppComponent wires them into the "rest"
 * connection handler:
 *
 *   - TrackingBodyDecoder raises a thread-local flag whenever anything reads the request
 *     body (every oatpp read path — readBodyToString, readBodyToDto, transferBody —
 *     funnels through the connection handler's BodyDecoder)
 *   - RequestBodyDrainRequestInterceptor lowers the flag as each request starts
 *   - RequestBodyDrainResponseInterceptor, which runs after the endpoint, discards the
 *     body if the request declared one and nothing consumed it
 *
 * The thread-local is safe because HttpConnectionHandler dedicates a thread to each
 * connection and handles its requests strictly one at a time; none of our endpoints are
 * async. If we ever move to AsyncHttpConnectionHandler this scheme must be rethought.
 */

class TrackingBodyDecoder : public oatpp::web::protocol::http::incoming::SimpleBodyDecoder {
  public:
    // True once the current request's body has been (or is being) read on this thread.
    // The flag is set at decode entry: if a read fails partway through, the connection
    // is broken and oatpp closes it anyway, so partial reads never need draining.
    static thread_local bool bodyWasRead;

    void decode(const oatpp::web::protocol::http::Headers &headers, oatpp::data::stream::InputStream *bodyStream,
                oatpp::data::stream::WriteCallback *writeCallback,
                oatpp::data::stream::IOStream *connection) const override;

    oatpp::async::CoroutineStarter
    decodeAsync(const oatpp::web::protocol::http::Headers &headers,
                const std::shared_ptr<oatpp::data::stream::InputStream> &bodyStream,
                const std::shared_ptr<oatpp::data::stream::WriteCallback> &writeCallback,
                const std::shared_ptr<oatpp::data::stream::IOStream> &connection) const override;
};

class RequestBodyDrainRequestInterceptor : public oatpp::web::server::interceptor::RequestInterceptor {
  public:
    std::shared_ptr<OutgoingResponse> intercept(const std::shared_ptr<IncomingRequest> &request) override;
};

class RequestBodyDrainResponseInterceptor : public oatpp::web::server::interceptor::ResponseInterceptor {
  public:
    std::shared_ptr<OutgoingResponse> intercept(const std::shared_ptr<IncomingRequest> &request,
                                                const std::shared_ptr<OutgoingResponse> &response) override;
};

} // namespace creatures::ws
