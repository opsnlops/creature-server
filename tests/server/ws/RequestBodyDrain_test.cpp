#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <oatpp/core/data/stream/BufferStream.hpp>
#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/web/protocol/http/incoming/Request.hpp>
#include <oatpp/web/protocol/http/outgoing/Response.hpp>

#include "server/ws/RequestBodyDrain.h"
#include "server/ws/controller/ControllerUtils.h"

// Regression tests for issue #142: an endpoint with no BODY_* macro never reads the
// request body, and oatpp 1.3 leaves those bytes in the connection's buffered stream —
// so the next request on the keep-alive connection parses as "{}GET ...". The drain
// interceptor must consume exactly the unread body, and must not touch the stream when
// the endpoint already read it (a double read would eat the next request instead).

namespace creatures ::ws {

namespace {

using oatpp::web::protocol::http::Header;
using oatpp::web::protocol::http::Headers;
using oatpp::web::protocol::http::RequestStartingLine;
using oatpp::web::protocol::http::Status;
using oatpp::web::protocol::http::incoming::Request;
using oatpp::web::protocol::http::outgoing::Response;

// What a poisoned connection would misparse as the next request's start line
constexpr const char *kNextRequest = "GET /api/v1/whatever HTTP/1.1\r\n";

std::shared_ptr<Request> makeRequest(const Headers &headers,
                                     const std::shared_ptr<oatpp::data::stream::BufferInputStream> &stream) {
    RequestStartingLine startingLine;
    startingLine.method = "POST";
    startingLine.path = "/api/v1/animation/dialog/music/generated/123/promote";
    startingLine.protocol = "HTTP/1.1";
    return Request::createShared(nullptr, startingLine, headers, stream, std::make_shared<TrackingBodyDecoder>());
}

std::string readRemaining(const std::shared_ptr<oatpp::data::stream::BufferInputStream> &stream) {
    std::string result;
    char buffer[64];
    while (true) {
        auto count = stream->readSimple(buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        result.append(buffer, static_cast<size_t>(count));
    }
    return result;
}

std::shared_ptr<Response> runInterceptors(const std::shared_ptr<Request> &request, bool endpointReadsBody) {
    RequestBodyDrainRequestInterceptor requestInterceptor;
    EXPECT_EQ(requestInterceptor.intercept(request), nullptr);

    if (endpointReadsBody) {
        request->readBodyToString();
    }

    RequestBodyDrainResponseInterceptor responseInterceptor;
    auto response = Response::createShared(Status::CODE_200, nullptr);
    auto intercepted = responseInterceptor.intercept(request, response);
    EXPECT_EQ(intercepted, response);
    return intercepted;
}

TEST(RequestBodyDrain, drainsUnreadBodySoNextRequestParsesCleanly) {
    Headers headers;
    headers.put(Header::CONTENT_LENGTH, "2");
    auto stream =
        std::make_shared<oatpp::data::stream::BufferInputStream>(oatpp::String(std::string("{}") + kNextRequest));

    runInterceptors(makeRequest(headers, stream), false);

    EXPECT_EQ(readRemaining(stream), kNextRequest);
}

TEST(RequestBodyDrain, leavesStreamAloneWhenEndpointAlreadyReadBody) {
    Headers headers;
    headers.put(Header::CONTENT_LENGTH, "2");
    auto stream =
        std::make_shared<oatpp::data::stream::BufferInputStream>(oatpp::String(std::string("{}") + kNextRequest));

    runInterceptors(makeRequest(headers, stream), true);

    EXPECT_EQ(readRemaining(stream), kNextRequest);
}

TEST(RequestBodyDrain, doesNothingWhenRequestHasNoBody) {
    Headers headers;
    auto stream = std::make_shared<oatpp::data::stream::BufferInputStream>(oatpp::String(kNextRequest));

    runInterceptors(makeRequest(headers, stream), false);

    EXPECT_EQ(readRemaining(stream), kNextRequest);
}

TEST(RequestBodyDrain, doesNothingWhenContentLengthIsZero) {
    Headers headers;
    headers.put(Header::CONTENT_LENGTH, "0");
    auto stream = std::make_shared<oatpp::data::stream::BufferInputStream>(oatpp::String(kNextRequest));

    runInterceptors(makeRequest(headers, stream), false);

    EXPECT_EQ(readRemaining(stream), kNextRequest);
}

TEST(RequestBodyDrain, drainsUnreadChunkedBody) {
    Headers headers;
    headers.put(Header::TRANSFER_ENCODING, "chunked");
    auto stream = std::make_shared<oatpp::data::stream::BufferInputStream>(
        oatpp::String(std::string("2\r\n{}\r\n0\r\n\r\n") + kNextRequest));

    runInterceptors(makeRequest(headers, stream), false);

    EXPECT_EQ(readRemaining(stream), kNextRequest);
}

TEST(LimitedStringWriteCallback, ReturnsBodiesWithinTheLimit) {
    LimitedStringWriteCallback callback(4);
    oatpp::async::Action action;

    EXPECT_EQ(callback.write("test", 4, action), 4);
    EXPECT_EQ(callback.takeBody(), "test");
}

TEST(LimitedStringWriteCallback, RejectsAChunkThatCrossesTheLimit) {
    LimitedStringWriteCallback callback(4);
    oatpp::async::Action action;
    ASSERT_EQ(callback.write("abc", 3, action), 3);

    try {
        callback.write("de", 2, action);
        FAIL() << "expected an HTTP 413";
    } catch (oatpp::web::protocol::http::HttpError &error) {
        EXPECT_EQ(error.getInfo().status.code, 413);
        EXPECT_EQ(callback.acceptedBytes(), 3u);
        EXPECT_EQ(callback.attemptedBytes(), 5u);
    }
}

} // namespace

} // namespace creatures::ws
