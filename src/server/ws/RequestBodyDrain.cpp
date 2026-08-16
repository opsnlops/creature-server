
#include "RequestBodyDrain.h"

#include <cstdlib>

#include "server/namespace-stuffs.h"

namespace creatures ::ws {

namespace {

// Swallows whatever the decoder feeds it. Used to consume an unread body without
// buffering it anywhere.
class DiscardingWriteCallback : public oatpp::data::stream::WriteCallback {
  public:
    oatpp::v_io_size write(const void *data, v_buff_size count, oatpp::async::Action &action) override {
        (void)data;
        (void)action;
        return count;
    }
};

bool requestDeclaresBody(const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request) {
    using oatpp::web::protocol::http::Header;

    if (request->getHeader(Header::TRANSFER_ENCODING)) {
        return true;
    }

    auto contentLength = request->getHeader(Header::CONTENT_LENGTH);
    if (!contentLength) {
        return false;
    }
    return std::strtoll(contentLength->c_str(), nullptr, 10) > 0;
}

} // namespace

thread_local bool TrackingBodyDecoder::bodyWasRead = false;

void TrackingBodyDecoder::decode(const oatpp::web::protocol::http::Headers &headers,
                                 oatpp::data::stream::InputStream *bodyStream,
                                 oatpp::data::stream::WriteCallback *writeCallback,
                                 oatpp::data::stream::IOStream *connection) const {
    bodyWasRead = true;
    SimpleBodyDecoder::decode(headers, bodyStream, writeCallback, connection);
}

oatpp::async::CoroutineStarter
TrackingBodyDecoder::decodeAsync(const oatpp::web::protocol::http::Headers &headers,
                                 const std::shared_ptr<oatpp::data::stream::InputStream> &bodyStream,
                                 const std::shared_ptr<oatpp::data::stream::WriteCallback> &writeCallback,
                                 const std::shared_ptr<oatpp::data::stream::IOStream> &connection) const {
    // We only run the synchronous connection handler; this exists to satisfy the
    // interface. A thread-local can't track consumption across coroutine hops, so the
    // drain logic would need a redesign before any async endpoint is added.
    bodyWasRead = true;
    return SimpleBodyDecoder::decodeAsync(headers, bodyStream, writeCallback, connection);
}

std::shared_ptr<oatpp::web::protocol::http::outgoing::Response>
RequestBodyDrainRequestInterceptor::intercept(const std::shared_ptr<IncomingRequest> &request) {
    (void)request;
    TrackingBodyDecoder::bodyWasRead = false;
    return nullptr;
}

std::shared_ptr<oatpp::web::protocol::http::outgoing::Response>
RequestBodyDrainResponseInterceptor::intercept(const std::shared_ptr<IncomingRequest> &request,
                                               const std::shared_ptr<OutgoingResponse> &response) {
    if (request && !TrackingBodyDecoder::bodyWasRead && requestDeclaresBody(request)) {
        debug("draining unread request body for {} {}", std::string(request->getStartingLine().method.toString()),
              std::string(request->getStartingLine().path.toString()));
        auto sink = std::make_shared<DiscardingWriteCallback>();
        // Goes through TrackingBodyDecoder, so a failed drain (broken connection) throws
        // and oatpp closes the connection instead of reusing it half-read.
        request->transferBody(sink);
    }
    return response;
}

} // namespace creatures::ws
