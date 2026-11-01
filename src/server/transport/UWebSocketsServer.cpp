#include "UWebSocketsServer.h"

#include <App.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "blockingconcurrentqueue.h"

#include "Version.h"
#include "api/AnimationRequests.h"
#include "api/CreatureRequests.h"
#include "api/DialogContracts.h"
#include "api/DialogStreamContracts.h"
#include "api/FixtureRequests.h"
#include "api/JsonResponse.h"
#include "api/MusicContracts.h"
#include "api/PlaylistRequests.h"
#include "api/SoundRequests.h"
#include "api/VoiceContracts.h"
#include "api/WebSocketEnvelope.h"
#include "model/Animation.h"
#include "model/Notice.h"
#include "model/Playlist.h"
#include "server/config.h"
#include "server/metrics/counters.h"
#include "server/namespace-stuffs.h"
#include "server/transport/AnimationHandlers.h"
#include "server/transport/ApiDocumentation.h"
#include "server/transport/ApplicationExecutor.h"
#include "server/transport/CreatureReadHandlers.h"
#include "server/transport/DialogHandlers.h"
#include "server/transport/DialogScriptHandlers.h"
#include "server/transport/DialogStreamHandlers.h"
#include "server/transport/DocumentHandlers.h"
#include "server/transport/FixtureReadHandlers.h"
#include "server/transport/FixtureWriteHandlers.h"
#include "server/transport/HttpTypes.h"
#include "server/transport/MediaFileHandlers.h"
#include "server/transport/MediaHandlers.h"
#include "server/transport/MusicHandlers.h"
#include "server/transport/OperationalHandlers.h"
#include "server/transport/PlaylistHandlers.h"
#include "server/transport/RequestRegistry.h"
#include "server/transport/UploadHandlers.h"
#include "server/ws/dto/websocket/MessageTypes.h"
#include "server/ws/messaging/MessageProcessor.h"
#include "server/ws/service/MetricsService.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/environment.h"
#include "util/helpers.h"
#include "util/threadName.h"

namespace creatures {
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;
} // namespace creatures

namespace creatures::transport {
namespace {

constexpr std::size_t UNEXPECTED_BODY_LIMIT = 1024 * 1024;
constexpr std::size_t APPLICATION_WORKER_COUNT = 4;
constexpr std::size_t APPLICATION_QUEUE_LIMIT = 8;
// File streaming: one read in flight per response, so the read queue is
// bounded by the connection limit (passed in at construction).
constexpr std::size_t FILE_CHUNK_BYTES = 256 * 1024;
constexpr std::size_t FILE_WORKER_COUNT = 2;
// Never trust an advertised Content-Length for allocation: a client can
// announce a route's full limit (1 GiB for uploads) and then send nothing.
constexpr std::size_t BODY_RESERVE_CAP_BYTES = 64 * 1024;
constexpr std::size_t WEBSOCKET_INBOUND_MESSAGE_LIMIT = 64 * 1024;
constexpr std::size_t WEBSOCKET_CONNECTION_MAILBOX_LIMIT = 16;
constexpr std::size_t WEBSOCKET_BACKPRESSURE_LIMIT = 256 * 1024;
constexpr std::string_view WEBSOCKET_BROADCAST_TOPIC = "creature.broadcast";
constexpr std::string_view ROOT_BODY =
    "<html lang='en'>"
    "  <head>"
    "    <meta charset=utf-8/>"
    "  </head>"
    "  <body>"
    "    <h1>April's Creature Workshop</h1>"
    "    <p>This is the server that controls everything. <a href='/api/docs'>Browse the API</a>!</p>"
    "  </body>"
    "</html>";

using ResponseProducer = std::function<PreparedResponse(const std::shared_ptr<creatures::OperationSpan> &parentSpan)>;
using BodyResponseProducer = std::function<PreparedResponse(
    const std::string &body, const std::shared_ptr<creatures::OperationSpan> &parentSpan)>;

struct PendingWebSocketMessage {
    std::string payload;
    uint64_t sequence{0};
};

struct WebSocketSession {
    uint64_t connectionId{0};
    uint64_t nextSequence{0};
    bool processing{false};
    std::deque<PendingWebSocketMessage> mailbox;
    std::string triggerTraceId;
    std::string triggerSpanId;
};

using CreatureWebSocket = uWS::WebSocket<false, true, WebSocketSession>;

std::pair<std::string, std::string> traceIdsFromTraceparent(const std::string_view traceparent) {
    if (traceparent.size() != 55 || traceparent[2] != '-' || traceparent[35] != '-' || traceparent[52] != '-') {
        return {};
    }
    return {std::string(traceparent.substr(3, 32)), std::string(traceparent.substr(36, 16))};
}

std::optional<std::string>
processWebSocketMessage(const std::shared_ptr<creatures::ws::MessageProcessor> &messageProcessor,
                        const PendingWebSocketMessage &message,
                        const creatures::ws::WebSocketMessageMetadata &metadata) {
    try {
        const auto parsedMessage = JsonParser::parseApiJsonString(message.payload, "WebSocket message");
        if (parsedMessage.isSuccess()) {
            messageProcessor->processIncomingMessage(parsedMessage.getValue().value(), message.payload, metadata);
            return std::nullopt;
        }

        spdlog::warn("Rejected inbound uWebSockets message: {}", parsedMessage.getError()->getMessage());
        const Notice notice{getCurrentTimeISO8601(), "Dropped malformed WebSocket message."};
        return api::serializeWebSocketEnvelope(toString(creatures::ws::MessageType::Notice), noticeToJson(notice));
    } catch (const std::exception &error) {
        spdlog::warn("Exception while processing inbound uWebSockets message for client {}: {}", metadata.connectionId,
                     error.what());
    } catch (...) {
        spdlog::warn("Unknown exception while processing inbound uWebSockets message for client {}",
                     metadata.connectionId);
    }
    return std::nullopt;
}

class LoopDispatcher {
  public:
    explicit LoopDispatcher(uWS::Loop *loop) : loop_(loop) {}

    bool post(std::function<void()> callback) {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            return false;
        }
        loop_->defer(std::move(callback));
        return true;
    }

    void close() {
        std::lock_guard lock(mutex_);
        accepting_ = false;
    }

  private:
    uWS::Loop *loop_;
    std::mutex mutex_;
    bool accepting_{true};
};

struct DrainState {
    RequestToken token;
    std::size_t receivedBytes{0};
    bool completed{false};
    bool aborted{false};
};

struct BodyState : DrainState {
    std::string body;
};

class ConnectionAdmission {
  public:
    ConnectionAdmission(const std::size_t maximumConnections, const std::size_t maximumConnectionsPerPeer)
        : maximumConnections_(maximumConnections), maximumConnectionsPerPeer_(maximumConnectionsPerPeer) {}

    void update(uWS::HttpResponse<false> *response, const int change) {
        if (change > 0) {
            opened(response);
        } else if (change < 0) {
            closed(response);
        }
    }

    void beginShutdown() { accepting_ = false; }

  private:
    struct Connection {
        std::string peer;
        bool admitted{false};
    };

    void opened(uWS::HttpResponse<false> *response) {
        std::string peer(response->getRemoteAddressAsText());
        if (peer.empty()) {
            peer = "unknown";
        }

        const auto peerIterator = connectionsPerPeer_.find(peer);
        const auto peerConnections = peerIterator == connectionsPerPeer_.end() ? std::size_t{0} : peerIterator->second;
        const bool admitted =
            accepting_ && activeConnections_ < maximumConnections_ && peerConnections < maximumConnectionsPerPeer_;
        connections_.insert_or_assign(response, Connection{.peer = peer, .admitted = admitted});
        if (!admitted) {
            response->close();
            return;
        }

        ++activeConnections_;
        connectionsPerPeer_[peer] = peerConnections + 1;
    }

    void closed(uWS::HttpResponse<false> *response) {
        const auto connection = connections_.find(response);
        if (connection == connections_.end()) {
            return;
        }
        if (connection->second.admitted) {
            if (activeConnections_ > 0) {
                --activeConnections_;
            }
            const auto peer = connectionsPerPeer_.find(connection->second.peer);
            if (peer != connectionsPerPeer_.end()) {
                if (peer->second > 1) {
                    --peer->second;
                } else {
                    connectionsPerPeer_.erase(peer);
                }
            }
        }
        connections_.erase(connection);
    }

    std::size_t maximumConnections_;
    std::size_t maximumConnectionsPerPeer_;
    std::size_t activeConnections_{0};
    bool accepting_{true};
    std::unordered_map<void *, Connection> connections_;
    std::unordered_map<std::string, std::size_t> connectionsPerPeer_;
};

std::string statusLine(const int statusCode) {
    switch (statusCode) {
    case 101:
        return "101 Switching Protocols";
    case 200:
        return "200 OK";
    case 201:
        return "201 Created";
    case 202:
        return "202 Accepted";
    case 204:
        return "204 No Content";
    case 206:
        return "206 Partial Content";
    case 301:
        return "301 Moved Permanently";
    case 302:
        return "302 Found";
    case 304:
        return "304 Not Modified";
    case 400:
        return "400 Bad Request";
    case 401:
        return "401 Unauthorized";
    case 403:
        return "403 Forbidden";
    case 404:
        return "404 Not Found";
    case 405:
        return "405 Method Not Allowed";
    case 409:
        return "409 Conflict";
    case 413:
        return "413 Payload Too Large";
    case 416:
        return "416 Range Not Satisfiable";
    case 422:
        return "422 Unprocessable Content";
    case 429:
        return "429 Too Many Requests";
    case 499:
        return "499 Client Closed Request";
    case 500:
        return "500 Internal Server Error";
    case 503:
        return "503 Service Unavailable";
    default:
        return fmt::format("{} Unknown", statusCode);
    }
}

PreparedResponse statusResponse(const int statusCode, std::string message) {
    return PreparedResponse::json(statusCode, api::jsonToString(api::statusResponseToJson(
                                                  api::makeStatusResponse(statusCode, std::move(message)))));
}

PreparedResponse produceResponse(ResponseProducer &producer,
                                 const std::shared_ptr<creatures::OperationSpan> &applicationSpan) {
    try {
        return producer(applicationSpan);
    } catch (const std::exception &error) {
        if (applicationSpan) {
            applicationSpan->setAttribute("error.type", "UnhandledException");
            applicationSpan->setAttribute("error.message", error.what());
            applicationSpan->recordException(error);
            applicationSpan->setError(error.what());
        }
        spdlog::error("Unhandled uWebSockets route exception: {}", error.what());
    } catch (...) {
        if (applicationSpan) {
            applicationSpan->setAttribute("error.type", "UnknownException");
            applicationSpan->setAttribute("error.message", "Unknown route exception");
            applicationSpan->setError("Unknown route exception");
        }
        spdlog::error("Unhandled unknown uWebSockets route exception");
    }
    return statusResponse(500, "Internal server error");
}

void writePreparedResponse(uWS::HttpResponse<false> *response, PreparedResponse prepared, bool headOnly,
                           const std::shared_ptr<creatures::RequestSpan> &span, bool closeConnection = false);

struct FileStreamContext {
    RequestToken token;
    std::string path;
    std::uint64_t size{0};
    // Loop-owned: the file offset the current chunk starts at, its length,
    // and whether a read is outstanding.
    std::uint64_t chunkStart{0};
    std::size_t chunkLength{0};
    bool readInFlight{false};
    // One buffer per stream, allocated once. The file worker fills it while
    // `readInFlight` is true and the loop reads it otherwise; the hand-off in
    // each direction goes through LoopDispatcher, which is the synchronisation.
    std::vector<char> buffer;
    // Touched only by the file worker, and only while a read is in flight.
    std::ifstream stream;
};

/**
 * Streams a FilePayload to the client in bounded chunks. The loop never reads
 * the file: every chunk is read on a small dedicated executor and posted back,
 * so a slow disk (or a FIFO masquerading as a WAV) cannot stall the loop. The
 * request stays in the RequestRegistry until the last byte is accepted by the
 * socket, so a client disconnect cancels it the same way as any response.
 */
class FileStreamer {
  public:
    /**
     * `maximumStreams` bounds the read queue. Each stream has at most one
     * read outstanding, so a queue as large as the connection limit can never
     * overflow and an admitted stream is never torn down for lack of a slot.
     */
    FileStreamer(RequestRegistry &registry, LoopDispatcher &dispatcher, const std::size_t maximumStreams)
        : registry_(registry), dispatcher_(dispatcher), executor_(FILE_WORKER_COUNT, maximumStreams) {
        registry_.setFileStreamStarter(
            [this](const RequestToken token, const PreparedResponse &prepared) { begin(token, prepared); });
    }
    ~FileStreamer() { registry_.setFileStreamStarter({}); }

    void requestStop() { executor_.requestStop(); }
    void join() { executor_.join(); }

    /** Loop thread only. The response must still be registered under `token`. */
    void begin(const RequestToken token, const PreparedResponse &prepared) {
        const auto registered = registry_.peek(token);
        if (!registered.has_value()) {
            return;
        }
        auto *response = static_cast<uWS::HttpResponse<false> *>(registered->response);
        auto context = std::make_shared<FileStreamContext>();
        context->token = token;
        context->path = prepared.file->path;
        context->size = prepared.file->size;
        context->buffer.resize(static_cast<std::size_t>(std::min<std::uint64_t>(FILE_CHUNK_BYTES, context->size)));
        if (const auto span = registry_.span(token)) {
            span->setAttribute("http.response.body.size", static_cast<int64_t>(context->size));
            if (!prepared.contentType.empty()) {
                span->setAttribute("http.response.content_type", prepared.contentType);
            }
            span->setAttribute("transport.response.mode", "file_stream");
        }
        response->cork([response, &prepared] {
            response->writeStatus(statusLine(prepared.statusCode));
            if (!prepared.contentType.empty()) {
                response->writeHeader("Content-Type", prepared.contentType);
            }
            for (const auto &header : prepared.headers) {
                response->writeHeader(header.name, header.value);
            }
        });
        response->onWritable([this, context](uintmax_t) -> bool {
            const auto registered = registry_.peek(context->token);
            if (!registered.has_value() || context->readInFlight) {
                // Nothing to write right now; let uWS drain what it has.
                return true;
            }
            return writeChunk(context, static_cast<uWS::HttpResponse<false> *>(registered->response));
        });
        requestChunk(context);
    }

  private:
    void requestChunk(const std::shared_ptr<FileStreamContext> &context) {
        context->readInFlight = true;
        const auto offset = context->chunkStart;
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(FILE_CHUNK_BYTES, context->size - offset));
        const bool accepted = executor_.trySubmit([this, context, offset, length](const std::stop_token stopToken) {
            const bool failed = !readChunk(*context, offset, length);
            if (stopToken.stop_requested()) {
                return;
            }
            dispatcher_.post([this, context, length, failed] { onChunk(context, length, failed); });
        });
        if (!accepted) {
            // Unreachable while the queue bound equals the connection bound;
            // kept so a future misconfiguration fails loudly rather than hangs.
            fail(context, "file_read_rejected", "File read queue is saturated");
        }
    }

    /** File worker only. Fills the first `length` bytes of the context buffer. */
    static bool readChunk(FileStreamContext &context, const std::uint64_t offset, const std::size_t length) {
        if (!context.stream.is_open()) {
            context.stream.open(context.path, std::ios::binary);
            if (!context.stream) {
                return false;
            }
        }
        context.stream.clear();
        context.stream.seekg(static_cast<std::streamoff>(offset));
        if (!context.stream) {
            return false;
        }
        context.stream.read(context.buffer.data(), static_cast<std::streamsize>(length));
        // A short read means the file shrank underneath us or the disk failed;
        // either way the advertised Content-Length can no longer be honoured.
        return context.stream.gcount() == static_cast<std::streamsize>(length);
    }

    void onChunk(const std::shared_ptr<FileStreamContext> &context, const std::size_t length, const bool failed) {
        context->readInFlight = false;
        const auto registered = registry_.peek(context->token);
        if (!registered.has_value()) {
            return; // the client went away; the abort was already recorded
        }
        if (failed) {
            fail(context, "file_read_failed", "Unable to read the file while streaming it");
            return;
        }
        context->chunkLength = length;
        writeChunk(context, static_cast<uWS::HttpResponse<false> *>(registered->response));
    }

    /** Returns false only when the socket is backpressured mid-chunk. */
    bool writeChunk(const std::shared_ptr<FileStreamContext> &context, uWS::HttpResponse<false> *response) {
        const auto written = response->getWriteOffset();
        const auto within = written >= context->chunkStart ? written - context->chunkStart : 0;
        if (within >= context->chunkLength) {
            return true; // spurious writable event; nothing of this chunk is pending
        }
        std::string_view remaining(context->buffer.data(), context->chunkLength);
        remaining.remove_prefix(static_cast<std::size_t>(within));
        bool ok = false;
        bool done = false;
        response->cork([&] { std::tie(ok, done) = response->tryEnd(remaining, context->size); });
        if (done) {
            complete(context);
            return true;
        }
        if (ok) {
            context->chunkStart += context->chunkLength;
            context->chunkLength = 0;
            requestChunk(context);
            return true;
        }
        return false;
    }

    void complete(const std::shared_ptr<FileStreamContext> &context) {
        const auto registered = registry_.take(context->token);
        if (!registered.has_value()) {
            return;
        }
        if (registered->span) {
            registered->span->setAttribute("transport.outcome", "response_completed");
            registered->span->setHttpStatus(200);
        }
    }

    void fail(const std::shared_ptr<FileStreamContext> &context, const std::string &outcome,
              const std::string &message) {
        const auto registered = registry_.take(context->token);
        if (!registered.has_value()) {
            return;
        }
        spdlog::warn("File stream of {} failed at byte {}: {}", context->path, context->chunkStart, message);
        if (registered->span) {
            registered->span->setAttribute("transport.outcome", outcome);
            registered->span->setAttribute("error.type", outcome);
            registered->span->setAttribute("error.message", message);
            registered->span->setError(message);
        }
        // Headers are already on the wire, so the only honest signal left is a
        // truncated transfer: close the socket.
        static_cast<uWS::HttpResponse<false> *>(registered->response)->close();
    }

    RequestRegistry &registry_;
    LoopDispatcher &dispatcher_;
    ApplicationExecutor executor_; // last member: joined before anything above goes away
};

void completeRegisteredResponse(RequestRegistry &registry, const RequestToken token, PreparedResponse prepared,
                                const bool closeConnection = false) {
    if (prepared.file.has_value() && prepared.file->size > 0) {
        const auto registered = registry.peek(token);
        if (!registered.has_value()) {
            return;
        }
        if (!registered->headOnly) {
            if (registry.hasFileStreamStarter()) {
                registry.startFileStream(token, prepared);
                return;
            }
            prepared = statusResponse(500, "File streaming is unavailable");
        }
    }
    auto registered = registry.take(token);
    if (!registered.has_value()) {
        return;
    }
    writePreparedResponse(static_cast<uWS::HttpResponse<false> *>(registered->response), std::move(prepared),
                          registered->headOnly, registered->span, closeConnection);
}

void dispatchApplicationResponse(const RequestToken token, ResponseProducer producer, ApplicationExecutor &executor,
                                 LoopDispatcher &dispatcher, RequestRegistry &registry) {
    auto requestSpan = registry.span(token);
    auto applicationSpan = creatures::observability
                               ? creatures::observability->createChildOperationSpan("http.application", requestSpan)
                               : nullptr;
    const bool accepted = executor.trySubmit(
        [token, producer = std::move(producer), applicationSpan, &dispatcher,
         &registry](const std::stop_token stopToken) mutable {
            auto prepared = produceResponse(producer, applicationSpan);
            if (stopToken.stop_requested()) {
                if (applicationSpan) {
                    applicationSpan->setAttribute("transport.outcome", "shutdown_cancelled");
                    applicationSpan->setError("Server shut down before application work completed");
                }
                return;
            }
            if (!dispatcher.post([token, prepared = std::move(prepared), &registry]() mutable {
                    completeRegisteredResponse(registry, token, std::move(prepared));
                })) {
                if (applicationSpan) {
                    applicationSpan->setAttribute("transport.outcome", "defer_failed");
                    applicationSpan->setError("Transport loop closed before the response completed");
                }
            }
        },
        [applicationSpan] {
            if (applicationSpan) {
                applicationSpan->setAttribute("transport.outcome", "abandoned");
                applicationSpan->setError("Application work was abandoned during shutdown");
            }
        });
    if (!accepted) {
        if (requestSpan) {
            requestSpan->setAttribute("transport.outcome", "queue_saturated");
            requestSpan->setAttribute("error.type", "QueueSaturated");
            requestSpan->setAttribute("error.code", "application_queue_full");
            requestSpan->setError("Application work queue is full");
        }
        completeRegisteredResponse(registry, token, statusResponse(503, "Application work queue is full"));
    }
}

std::shared_ptr<creatures::RequestSpan> createRequestSpan(uWS::HttpResponse<false> *response, uWS::HttpRequest *request,
                                                          std::string_view method, std::string_view route,
                                                          std::string_view endpoint, std::string_view handler) {
    if (creatures::metrics) {
        creatures::metrics->incrementRestRequestsProcessed();
    }
    if (!creatures::observability) {
        return nullptr;
    }

    const std::string methodString(method);
    const std::string routeString(route);
    auto span = creatures::observability->createRequestSpan(methodString + " " + routeString, methodString, routeString,
                                                            std::string(request->getHeader("traceparent")));
    if (!span) {
        return nullptr;
    }

    span->setAttribute("http.route", routeString);
    span->setAttribute("http.method", methodString);
    span->setAttribute("http.target", std::string(request->getUrl()));
    span->setAttribute("http.protocol_version", "1.1");
    span->setAttribute("endpoint.name", std::string(endpoint));
    span->setAttribute("controller.name", std::string(handler));
    span->setAttribute("transport.framework", "uwebsockets");
    span->setAttribute("network.peer.address", std::string(response->getRemoteAddressAsText()));

    const auto userAgent = request->getHeader("user-agent");
    if (!userAgent.empty()) {
        span->setAttribute("http.user_agent", std::string(userAgent));
    }
    const auto host = request->getHeader("host");
    if (!host.empty()) {
        span->setAttribute("http.host", std::string(host));
    }
    const auto contentLength = request->getHeader("content-length");
    if (!contentLength.empty()) {
        span->setAttribute("http.request_content_length", std::string(contentLength));
    }
    return span;
}

void writePreparedResponse(uWS::HttpResponse<false> *response, PreparedResponse prepared, const bool headOnly,
                           const std::shared_ptr<creatures::RequestSpan> &span, const bool closeConnection) {
    if (span) {
        span->setAttribute("http.response.body.size", static_cast<int64_t>(prepared.contentLength()));
        if (!prepared.contentType.empty()) {
            span->setAttribute("http.response.content_type", prepared.contentType);
        }
        span->setAttribute("transport.outcome", "response_completed");
        if (prepared.statusCode >= 400) {
            span->setAttribute("error.type", "HttpError");
            span->setAttribute("error.code", static_cast<int64_t>(prepared.statusCode));
            span->setError("HTTP request failed");
        }
        span->setHttpStatus(prepared.statusCode);
    }

    response->cork([response, prepared = std::move(prepared), headOnly, closeConnection]() mutable {
        response->writeStatus(statusLine(prepared.statusCode));
        if (!prepared.contentType.empty()) {
            response->writeHeader("Content-Type", prepared.contentType);
        }
        for (const auto &header : prepared.headers) {
            response->writeHeader(header.name, header.value);
        }
        if (headOnly) {
            response->endWithoutBody(static_cast<std::size_t>(prepared.contentLength()), closeConnection);
        } else {
            response->end(prepared.body, closeConnection);
        }
    });
}

std::optional<std::size_t> advertisedContentLength(uWS::HttpRequest *request) {
    const auto header = request->getHeader("content-length");
    if (header.empty()) {
        return std::nullopt;
    }
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(header.data(), header.data() + header.size(), value);
    if (error != std::errc{} || end != header.data() + header.size()) {
        return std::nullopt;
    }
    return value;
}

void runBodylessRoute(uWS::HttpResponse<false> *response, uWS::HttpRequest *request, std::string_view method,
                      std::string_view route, std::string_view endpoint, std::string_view handler, bool headOnly,
                      ResponseProducer producer, RequestRegistry &registry, ApplicationExecutor *executor = nullptr,
                      LoopDispatcher *dispatcher = nullptr) {
    auto span = createRequestSpan(response, request, method, route, endpoint, handler);
    auto state = std::make_shared<DrainState>();
    state->token = registry.add(response, std::move(span), headOnly);
    response->onAborted([state, &registry] {
        state->aborted = true;
        registry.abort(state->token, "client_aborted", "Client aborted before the response completed", 499);
    });

    const auto contentLength = advertisedContentLength(request);
    const bool chunked = !request->getHeader("transfer-encoding").empty();

    if (contentLength.has_value() && contentLength.value() > UNEXPECTED_BODY_LIMIT) {
        auto requestSpan = registry.span(state->token);
        if (requestSpan) {
            requestSpan->setAttribute("http.request.body.limit", static_cast<int64_t>(UNEXPECTED_BODY_LIMIT));
            requestSpan->setAttribute("transport.outcome", "body_too_large");
        }
        completeRegisteredResponse(registry, state->token, statusResponse(413, "Request body is too large"), true);
        return;
    }

    const bool hasBody = (contentLength.has_value() && contentLength.value() > 0) || chunked;
    if (!hasBody) {
        if (executor != nullptr && dispatcher != nullptr) {
            dispatchApplicationResponse(state->token, std::move(producer), *executor, *dispatcher, registry);
        } else {
            completeRegisteredResponse(registry, state->token, produceResponse(producer, nullptr));
        }
        return;
    }

    response->onDataV2([state, producer = std::move(producer), executor, dispatcher,
                        &registry](std::string_view chunk, std::uint64_t remaining) mutable {
        if (state->aborted || state->completed) {
            return;
        }
        if (chunk.size() > UNEXPECTED_BODY_LIMIT - state->receivedBytes) {
            state->completed = true;
            auto requestSpan = registry.span(state->token);
            if (requestSpan) {
                requestSpan->setAttribute("http.request.body.limit", static_cast<int64_t>(UNEXPECTED_BODY_LIMIT));
                requestSpan->setAttribute("http.request.body.size",
                                          static_cast<int64_t>(state->receivedBytes + chunk.size()));
                requestSpan->setAttribute("transport.outcome", "body_too_large");
            }
            completeRegisteredResponse(registry, state->token, statusResponse(413, "Request body is too large"), true);
            return;
        }
        state->receivedBytes += chunk.size();
        if (remaining == 0) {
            state->completed = true;
            auto requestSpan = registry.span(state->token);
            if (requestSpan) {
                requestSpan->setAttribute("http.request.body_drained", static_cast<int64_t>(state->receivedBytes));
            }
            if (executor != nullptr && dispatcher != nullptr) {
                dispatchApplicationResponse(state->token, std::move(producer), *executor, *dispatcher, registry);
            } else {
                completeRegisteredResponse(registry, state->token, produceResponse(producer, nullptr));
            }
        }
    });
}

void runBodyRoute(uWS::HttpResponse<false> *response, uWS::HttpRequest *request, std::string_view method,
                  std::string_view route, std::string_view endpoint, std::string_view handler,
                  const std::size_t maximumBodyBytes, BodyResponseProducer producer, RequestRegistry &registry,
                  ApplicationExecutor &executor, LoopDispatcher &dispatcher) {
    auto span = createRequestSpan(response, request, method, route, endpoint, handler);
    auto state = std::make_shared<BodyState>();
    state->token = registry.add(response, std::move(span), false);
    response->onAborted([state, &registry] {
        state->aborted = true;
        registry.abort(state->token, "client_aborted", "Client aborted before the response completed", 499);
    });

    const auto contentLengthHeader = request->getHeader("content-length");
    const auto contentLength = advertisedContentLength(request);
    if (!contentLengthHeader.empty() && !contentLength.has_value()) {
        completeRegisteredResponse(registry, state->token, statusResponse(400, "Content-Length is invalid"), true);
        return;
    }
    if (contentLength.has_value() && contentLength.value() > maximumBodyBytes) {
        auto requestSpan = registry.span(state->token);
        if (requestSpan) {
            requestSpan->setAttribute("http.request.body.limit", static_cast<int64_t>(maximumBodyBytes));
            requestSpan->setAttribute("http.request.body.size", static_cast<int64_t>(contentLength.value()));
            requestSpan->setAttribute("transport.outcome", "body_too_large");
        }
        completeRegisteredResponse(registry, state->token, statusResponse(413, "Request body is too large"), true);
        return;
    }
    if (contentLength.has_value()) {
        state->body.reserve(std::min<std::size_t>(contentLength.value(), BODY_RESERVE_CAP_BYTES));
    }

    auto dispatch = [state, producer = std::move(producer), &registry, &executor, &dispatcher]() mutable {
        auto body = std::move(state->body);
        ResponseProducer responseProducer = [body = std::move(body),
                                             producer = std::move(producer)](const auto &applicationSpan) mutable {
            return producer(body, applicationSpan);
        };
        dispatchApplicationResponse(state->token, std::move(responseProducer), executor, dispatcher, registry);
    };

    const bool chunked = !request->getHeader("transfer-encoding").empty();
    if ((!contentLength.has_value() || contentLength.value() == 0) && !chunked) {
        state->completed = true;
        dispatch();
        return;
    }

    response->onDataV2([state, maximumBodyBytes, dispatch = std::move(dispatch),
                        &registry](const std::string_view chunk, const std::uint64_t remaining) mutable {
        if (state->aborted || state->completed) {
            return;
        }
        if (chunk.size() > maximumBodyBytes - state->receivedBytes) {
            state->completed = true;
            auto requestSpan = registry.span(state->token);
            if (requestSpan) {
                requestSpan->setAttribute("http.request.body.limit", static_cast<int64_t>(maximumBodyBytes));
                requestSpan->setAttribute("http.request.body.size",
                                          static_cast<int64_t>(state->receivedBytes + chunk.size()));
                requestSpan->setAttribute("transport.outcome", "body_too_large");
            }
            completeRegisteredResponse(registry, state->token, statusResponse(413, "Request body is too large"), true);
            return;
        }
        state->body.append(chunk);
        state->receivedBytes += chunk.size();
        if (remaining == 0) {
            state->completed = true;
            if (auto requestSpan = registry.span(state->token)) {
                requestSpan->setAttribute("http.request.body.size", static_cast<int64_t>(state->receivedBytes));
            }
            dispatch();
        }
    });
}

PreparedResponse rootResponse() {
    return {.statusCode = 200,
            .contentType = "text/html",
            .body = std::string(ROOT_BODY),
            .headers = {{"Server", fmt::format("Creature-Server/{}.{}.{}", CREATURE_SERVER_VERSION_MAJOR,
                                               CREATURE_SERVER_VERSION_MINOR, CREATURE_SERVER_VERSION_PATCH)},
                        {"All-The-Birds-Sing-Words", "yes"},
                        {"And-The-Flowers-Croon", "of course"}}};
}

PreparedResponse healthResponse() { return statusResponse(200, "Server is operational"); }

PreparedResponse apiDocsResponse() {
    return {.statusCode = 200,
            .contentType = "text/html; charset=utf-8",
            .body = std::string(apiBrowserHtml()),
            .headers = {{"Content-Security-Policy",
                         "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src "
                         "'self'"}}};
}

PreparedResponse openApiResponse() { return PreparedResponse::json(200, openApiDocument()); }

PreparedResponse metricsResponse(const std::shared_ptr<creatures::OperationSpan> &span) {
    creatures::ws::MetricsService service;
    const auto result = service.getCountersFromOperation(span);
    if (!result.isSuccess()) {
        const auto error = result.getError().value();
        return statusResponse(serverErrorToStatusCode(error.getCode()), error.getMessage());
    }
    return PreparedResponse::json(200, api::jsonToString(systemCountersSnapshotToJson(result.getValue().value())));
}

} // namespace

UWebSocketsServer::UWebSocketsServer(const uint32_t maximumConnections, const uint32_t maximumConnectionsPerPeer)
    : maximumConnections_(maximumConnections), maximumConnectionsPerPeer_(maximumConnectionsPerPeer) {}

UWebSocketsServer::~UWebSocketsServer() { shutdown(); }

void UWebSocketsServer::start() {
    std::unique_lock lock(lifecycleMutex_);
    if (startState_ != StartState::NotStarted) {
        throw std::logic_error("uWebSockets transport can only be started once");
    }
    startState_ = StartState::Starting;
    thread_ = std::thread(&UWebSocketsServer::run, this);
    lifecycleCondition_.wait(
        lock, [this] { return startState_ == StartState::Running || startState_ == StartState::Failed; });
    if (startState_ == StartState::Failed) {
        const auto error = startError_;
        lock.unlock();
        if (thread_.joinable()) {
            thread_.join();
        }
        throw std::runtime_error(error);
    }
}

void UWebSocketsServer::shutdown() {
    {
        std::lock_guard lock(lifecycleMutex_);
        if (shutdownRequested_) {
            // Another caller has already posted the loop-owned close.
        } else {
            shutdownRequested_ = true;
            if (loop_ != nullptr && shutdownAction_) {
                auto *loop = static_cast<uWS::Loop *>(loop_);
                loop->defer([this] {
                    std::function<void()> action;
                    {
                        std::lock_guard callbackLock(lifecycleMutex_);
                        action = shutdownAction_;
                    }
                    if (action) {
                        action();
                    }
                });
            }
        }
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
}

void UWebSocketsServer::publishStartState(const StartState state, std::string error) {
    {
        std::lock_guard lock(lifecycleMutex_);
        startState_ = state;
        startError_ = std::move(error);
    }
    lifecycleCondition_.notify_all();
}

void UWebSocketsServer::run() {
    setThreadName("uWebSockets::run");
    try {
        const auto configuredPort = environmentToInt(SERVER_PORT_ENV, DEFAULT_SERVER_PORT);
        if (configuredPort < 1 || configuredPort > 65535) {
            publishStartState(StartState::Failed, "SERVER_PORT must be between 1 and 65535");
            return;
        }

        auto *loop = uWS::Loop::get();
        LoopDispatcher dispatcher(loop);
        ApplicationExecutor applicationExecutor(APPLICATION_WORKER_COUNT, APPLICATION_QUEUE_LIMIT);
        ConnectionAdmission admission(maximumConnections_, maximumConnectionsPerPeer_);
        RequestRegistry requestRegistry;
        FileStreamer fileStreamer(requestRegistry, dispatcher, maximumConnections_);
        uWS::App app;
        auto websocketMessageProcessor = std::make_shared<creatures::ws::MessageProcessor>(spdlog::default_logger());
        std::unordered_map<uint64_t, CreatureWebSocket *> webSockets;
        uint64_t nextWebSocketConnectionId = 1;
        uint64_t outboundWebSocketSequence = 0;
        std::size_t websocketConnectionCount = 0;
        std::jthread websocketBroadcastThread;
        std::function<void(uint64_t)> dispatchWebSocketMessage;

        dispatchWebSocketMessage = [&](const uint64_t connectionId) {
            const auto socketIterator = webSockets.find(connectionId);
            if (socketIterator == webSockets.end()) {
                return;
            }
            auto *webSocket = socketIterator->second;
            auto *session = webSocket->getUserData();
            if (session->processing || session->mailbox.empty()) {
                return;
            }

            auto message = std::move(session->mailbox.front());
            session->mailbox.pop_front();
            session->processing = true;
            const creatures::ws::WebSocketMessageMetadata metadata{
                .transportFramework = "uwebsockets",
                .connectionId = connectionId,
                .sequence = message.sequence,
                .triggerTraceId = session->triggerTraceId,
                .triggerSpanId = session->triggerSpanId,
            };

            const bool accepted = applicationExecutor.trySubmit(
                [connectionId, message = std::move(message), metadata, websocketMessageProcessor, &dispatcher,
                 &webSockets, &dispatchWebSocketMessage](const std::stop_token stopToken) mutable {
                    auto reply = processWebSocketMessage(websocketMessageProcessor, message, metadata);
                    if (stopToken.stop_requested()) {
                        return;
                    }
                    dispatcher.post(
                        [connectionId, reply = std::move(reply), &webSockets, &dispatchWebSocketMessage]() mutable {
                            const auto activeSocket = webSockets.find(connectionId);
                            if (activeSocket == webSockets.end()) {
                                return;
                            }
                            auto *socket = activeSocket->second;
                            auto *activeSession = socket->getUserData();
                            activeSession->processing = false;
                            if (reply.has_value()) {
                                const auto status = socket->send(*reply, uWS::OpCode::TEXT);
                                if (status != CreatureWebSocket::DROPPED && creatures::metrics) {
                                    creatures::metrics->incrementWebsocketMessagesSent();
                                }
                            }
                            dispatchWebSocketMessage(connectionId);
                        });
                });

            if (!accepted) {
                session->processing = false;
                session->mailbox.clear();
                spdlog::warn("Closing WebSocket client {} because the application executor is saturated", connectionId);
                webSocket->end(1013, "Server busy");
            }
        };
        {
            std::lock_guard lock(lifecycleMutex_);
            loop_ = loop;
            app_ = &app;
            shutdownAction_ = [&admission, &applicationExecutor, &fileStreamer, &requestRegistry, &dispatcher, &app,
                               &websocketBroadcastThread] {
                admission.beginShutdown();
                applicationExecutor.requestStop();
                fileStreamer.requestStop();
                websocketBroadcastThread.request_stop();
                auto cancelled = requestRegistry.cancelAll("shutdown_cancelled", "Server is shutting down", 503);
                for (auto &request : cancelled) {
                    if (request.response != nullptr) {
                        static_cast<uWS::HttpResponse<false> *>(request.response)->close();
                    }
                }
                dispatcher.close();
                app.close();
            };
        }

        app.filter([&admission](auto *response, const int change) { admission.update(response, change); })
            .get("/",
                 [&requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/", "root", "StaticController", false,
                         [](const auto &) { return rootResponse(); }, requestRegistry);
                 })
            .head("/",
                  [&requestRegistry](auto *response, auto *request) {
                      runBodylessRoute(
                          response, request, "HEAD", "/", "root", "StaticController", true,
                          [](const auto &) { return rootResponse(); }, requestRegistry);
                  })
            .get("/api/v1/health",
                 [&requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/health", "health", "StaticController", false,
                         [](const auto &) { return healthResponse(); }, requestRegistry);
                 })
            .head("/api/v1/health",
                  [&requestRegistry](auto *response, auto *request) {
                      runBodylessRoute(
                          response, request, "HEAD", "/api/v1/health", "health", "StaticController", true,
                          [](const auto &) { return healthResponse(); }, requestRegistry);
                  })
            .get("/api/docs",
                 [&requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/docs", "apiDocs", "StaticController", false,
                         [](const auto &) { return apiDocsResponse(); }, requestRegistry);
                 })
            .head("/api/docs",
                  [&requestRegistry](auto *response, auto *request) {
                      runBodylessRoute(
                          response, request, "HEAD", "/api/docs", "apiDocs", "StaticController", true,
                          [](const auto &) { return apiDocsResponse(); }, requestRegistry);
                  })
            .get("/api/docs/",
                 [&requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/docs/", "apiDocs", "StaticController", false,
                         [](const auto &) { return apiDocsResponse(); }, requestRegistry);
                 })
            .head("/api/docs/",
                  [&requestRegistry](auto *response, auto *request) {
                      runBodylessRoute(
                          response, request, "HEAD", "/api/docs/", "apiDocs", "StaticController", true,
                          [](const auto &) { return apiDocsResponse(); }, requestRegistry);
                  })
            .get("/api/openapi.json",
                 [&requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/openapi.json", "openApi", "StaticController", false,
                         [](const auto &) { return openApiResponse(); }, requestRegistry);
                 })
            .head("/api/openapi.json",
                  [&requestRegistry](auto *response, auto *request) {
                      runBodylessRoute(
                          response, request, "HEAD", "/api/openapi.json", "openApi", "StaticController", true,
                          [](const auto &) { return openApiResponse(); }, requestRegistry);
                  })
            .get("/api/v1/metric/counters",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/metric/counters", "counters", "MetricsController", false,
                         [](const auto &span) { return metricsResponse(span); }, requestRegistry, &applicationExecutor,
                         &dispatcher);
                 })
            .head("/api/v1/metric/counters",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodylessRoute(
                          response, request, "HEAD", "/api/v1/metric/counters", "counters", "MetricsController", true,
                          [](const auto &span) { return metricsResponse(span); }, requestRegistry, &applicationExecutor,
                          &dispatcher);
                  })
            .get("/api/v1/creature",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/creature", "getAllCreatures", "CreatureController", false,
                         [](const auto &span) { return listCreatures(span); }, requestRegistry, &applicationExecutor,
                         &dispatcher);
                 })
            .head("/api/v1/creature",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodylessRoute(
                          response, request, "HEAD", "/api/v1/creature", "getAllCreatures", "CreatureController", true,
                          [](const auto &span) { return listCreatures(span); }, requestRegistry, &applicationExecutor,
                          &dispatcher);
                  })
            .post("/api/v1/creature",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/creature", "upsertCreature", "CreatureController",
                          MAX_CREATURE_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return upsertCreature(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/creature/validate",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/creature/validate", "validateCreatureConfig",
                          "CreatureController", MAX_CREATURE_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return validateCreature(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/creature/register",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/creature/register", "registerCreature",
                          "CreatureController", api::MAX_REGISTER_CREATURE_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return registerCreature(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .get("/api/v1/creature/:creatureId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string creatureId(request->getParameter("creatureId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/creature/{creatureId}", "getCreature", "CreatureController",
                         false, [creatureId](const auto &span) { return getCreature(creatureId, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .head("/api/v1/creature/:creatureId",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string creatureId(request->getParameter("creatureId"));
                      runBodylessRoute(
                          response, request, "HEAD", "/api/v1/creature/{creatureId}", "getCreature",
                          "CreatureController", true,
                          [creatureId](const auto &span) { return getCreature(creatureId, span); }, requestRegistry,
                          &applicationExecutor, &dispatcher);
                  })
            .get("/api/v1/creature/:creatureId/export",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string creatureId(request->getParameter("creatureId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/creature/{creatureId}/export", "exportCreature",
                         "CreatureController", false,
                         [creatureId](const auto &span) { return exportCreature(creatureId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .head("/api/v1/creature/:creatureId/export",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string creatureId(request->getParameter("creatureId"));
                      runBodylessRoute(
                          response, request, "HEAD", "/api/v1/creature/{creatureId}/export", "exportCreature",
                          "CreatureController", true,
                          [creatureId](const auto &span) { return exportCreature(creatureId, span); }, requestRegistry,
                          &applicationExecutor, &dispatcher);
                  })
            .patch("/api/v1/creature/:creatureId/idle",
                   [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                       const std::string creatureId(request->getParameter("creatureId"));
                       runBodyRoute(
                           response, request, "PATCH", "/api/v1/creature/{creatureId}/idle", "setIdleEnabled",
                           "CreatureController", api::MAX_IDLE_TOGGLE_REQUEST_BODY_BYTES,
                           [creatureId](const std::string &body, const auto &span) {
                               return setCreatureIdle(creatureId, body, span);
                           },
                           requestRegistry, applicationExecutor, dispatcher);
                   })
            .get("/api/v1/fixture",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/fixture", "getAllFixtures", "DmxFixtureController", false,
                         [](const auto &span) { return listFixtures(span); }, requestRegistry, &applicationExecutor,
                         &dispatcher);
                 })
            .head("/api/v1/fixture",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodylessRoute(
                          response, request, "HEAD", "/api/v1/fixture", "getAllFixtures", "DmxFixtureController", true,
                          [](const auto &span) { return listFixtures(span); }, requestRegistry, &applicationExecutor,
                          &dispatcher);
                  })
            .post("/api/v1/fixture",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/fixture", "upsertFixture", "DmxFixtureController",
                          api::MAX_FIXTURE_CONFIG_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return upsertFixture(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/fixture/validate",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/fixture/validate", "validateFixtureConfig",
                          "DmxFixtureController", api::MAX_FIXTURE_CONFIG_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return validateFixture(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .get("/api/v1/fixture/:fixtureId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string fixtureId(request->getParameter("fixtureId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/fixture/{fixtureId}", "getFixture", "DmxFixtureController",
                         false, [fixtureId](const auto &span) { return getFixture(fixtureId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .head("/api/v1/fixture/:fixtureId",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string fixtureId(request->getParameter("fixtureId"));
                      runBodylessRoute(
                          response, request, "HEAD", "/api/v1/fixture/{fixtureId}", "getFixture",
                          "DmxFixtureController", true,
                          [fixtureId](const auto &span) { return getFixture(fixtureId, span); }, requestRegistry,
                          &applicationExecutor, &dispatcher);
                  })
            .del("/api/v1/fixture/:fixtureId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string fixtureId(request->getParameter("fixtureId"));
                     runBodylessRoute(
                         response, request, "DELETE", "/api/v1/fixture/{fixtureId}", "deleteFixture",
                         "DmxFixtureController", false,
                         [fixtureId](const auto &span) { return deleteFixture(fixtureId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .put("/api/v1/fixture/:fixtureId/universe",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string fixtureId(request->getParameter("fixtureId"));
                     runBodyRoute(
                         response, request, "PUT", "/api/v1/fixture/{fixtureId}/universe", "setFixtureUniverse",
                         "DmxFixtureController", api::MAX_FIXTURE_CONTROL_REQUEST_BODY_BYTES,
                         [fixtureId](const std::string &body, const auto &span) {
                             return setFixtureUniverse(fixtureId, body, span);
                         },
                         requestRegistry, applicationExecutor, dispatcher);
                 })
            .del("/api/v1/fixture/:fixtureId/universe",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string fixtureId(request->getParameter("fixtureId"));
                     runBodylessRoute(
                         response, request, "DELETE", "/api/v1/fixture/{fixtureId}/universe", "clearFixtureUniverse",
                         "DmxFixtureController", false,
                         [fixtureId](const auto &span) { return clearFixtureUniverse(fixtureId, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/fixture/:fixtureId/pattern/:patternId/trigger",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string fixtureId(request->getParameter("fixtureId"));
                      const std::string patternId(request->getParameter("patternId"));
                      runBodyRoute(
                          response, request, "POST", "/api/v1/fixture/{fixtureId}/pattern/{patternId}/trigger",
                          "triggerFixturePattern", "DmxFixtureController", api::MAX_FIXTURE_CONTROL_REQUEST_BODY_BYTES,
                          [fixtureId, patternId](const std::string &body, const auto &span) {
                              return triggerFixturePattern(fixtureId, patternId, body, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/fixture/:fixtureId/pattern/preview",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string fixtureId(request->getParameter("fixtureId"));
                      runBodyRoute(
                          response, request, "POST", "/api/v1/fixture/{fixtureId}/pattern/preview",
                          "previewFixturePattern", "DmxFixtureController", api::MAX_FIXTURE_CONTROL_REQUEST_BODY_BYTES,
                          [fixtureId](const std::string &body, const auto &span) {
                              return previewFixturePattern(fixtureId, body, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/fixture/:fixtureId/live",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string fixtureId(request->getParameter("fixtureId"));
                      runBodyRoute(
                          response, request, "POST", "/api/v1/fixture/{fixtureId}/live", "setFixtureLive",
                          "DmxFixtureController", api::MAX_FIXTURE_CONTROL_REQUEST_BODY_BYTES,
                          [fixtureId](const std::string &body, const auto &span) {
                              return setFixtureLive(fixtureId, body, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .get("/api/v1/playlist",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/playlist", "getAllPlaylists", "PlaylistController", false,
                         [](const auto &span) { return listPlaylists(span); }, requestRegistry, &applicationExecutor,
                         &dispatcher);
                 })
            .get("/api/v1/playlist/id/:playlistId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string playlistId(request->getParameter("playlistId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/playlist/id/{playlistId}", "getPlaylist",
                         "PlaylistController", false,
                         [playlistId](const auto &span) { return getPlaylist(playlistId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/playlist",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/playlist", "upsertPlaylist", "PlaylistController",
                          MAX_PLAYLIST_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return upsertPlaylist(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/playlist/start",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/playlist/start", "startPlaylist", "PlaylistController",
                          api::MAX_PLAYLIST_CONTROL_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return startPlaylist(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/playlist/stop",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/playlist/stop", "stopPlaylist", "PlaylistController",
                          api::MAX_PLAYLIST_CONTROL_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return stopPlaylist(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .get("/api/v1/playlist/status/:universe",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string universe(request->getParameter("universe"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/playlist/status/{universe}", "playlistStatus",
                         "PlaylistController", false,
                         [universe](const auto &span) { return getPlaylistStatus(universe, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/playlist/status",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/playlist/status", "getAllPlaylistStatuses",
                         "PlaylistController", false, [](const auto &span) { return listPlaylistStatuses(span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/job/:jobId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string jobId(request->getParameter("jobId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/job/{jobId}", "getJob", "JobController", false,
                         [jobId](const auto &span) { return getJob(jobId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/stage",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/stage", "listStages", "StageController", false,
                         [](const auto &span) { return listStages(span); }, requestRegistry, &applicationExecutor,
                         &dispatcher);
                 })
            .get("/api/v1/stage/:stageId/animations",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string stageId(request->getParameter("stageId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/stage/{stageId}/animations", "listStageAnimations",
                         "StageController", false,
                         [stageId](const auto &span) { return listStageAnimations(stageId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation", "listAllAnimations", "AnimationController",
                         false, [](const auto &span) { return listAnimations(span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation/ad-hoc",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/ad-hoc", "listAdHocAnimations",
                         "AnimationController", false, [](const auto &span) { return listAdHocAnimations(span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation/ad-hoc/:animationId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string id(request->getParameter("animationId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/ad-hoc/{animationId}", "getAdHocAnimation",
                         "AnimationController", false, [id](const auto &span) { return getAnimation(id, true, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation/:animationId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string id(request->getParameter("animationId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/{animationId}", "getAnimation",
                         "AnimationController", false, [id](const auto &span) { return getAnimation(id, false, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/animation",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation", "upsertAnimation", "AnimationController",
                          MAX_ANIMATION_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return upsertAnimation(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .del("/api/v1/animation/:animationId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string id(request->getParameter("animationId"));
                     runBodylessRoute(
                         response, request, "DELETE", "/api/v1/animation/{animationId}", "deleteAnimation",
                         "AnimationController", false, [id](const auto &span) { return deleteAnimation(id, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/animation/play",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/play", "playStoredAnimation",
                          "AnimationController", api::MAX_ANIMATION_CONTROL_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return playAnimation(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/generate-lipsync",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/generate-lipsync",
                          "generateLipSyncForAnimation", "AnimationController",
                          api::MAX_ANIMATION_CONTROL_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) {
                              return regenerateAnimationLipSync(body, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/ad-hoc",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/ad-hoc", "createAdHocAnimation",
                          "AnimationController", api::MAX_AD_HOC_SPEECH_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) {
                              return createAdHocAnimation(body, true, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/ad-hoc/prepare",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/ad-hoc/prepare", "prepareAdHocAnimation",
                          "AnimationController", api::MAX_AD_HOC_SPEECH_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) {
                              return createAdHocAnimation(body, false, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .get("/api/v1/stage/:stageId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string stageId(request->getParameter("stageId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/stage/{stageId}", "getStage", "StageController", false,
                         [stageId](const auto &span) { return getStage(stageId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/stage",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/stage", "createStage", "StageController", 1024 * 1024,
                          [](const std::string &body, const auto &span) { return createStage(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .put("/api/v1/stage/:stageId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string stageId(request->getParameter("stageId"));
                     runBodyRoute(
                         response, request, "PUT", "/api/v1/stage/{stageId}", "updateStage", "StageController",
                         1024 * 1024,
                         [stageId](const std::string &body, const auto &span) {
                             return updateStage(stageId, body, span);
                         },
                         requestRegistry, applicationExecutor, dispatcher);
                 })
            .del("/api/v1/stage/:stageId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string stageId(request->getParameter("stageId"));
                     runBodylessRoute(
                         response, request, "DELETE", "/api/v1/stage/{stageId}", "deleteStage", "StageController",
                         false, [stageId](const auto &span) { return deleteStage(stageId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/stage/:stageId/rerender",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string stageId(request->getParameter("stageId"));
                      runBodyRoute(
                          response, request, "POST", "/api/v1/stage/{stageId}/rerender", "rerenderStage",
                          "StageController", 4096,
                          [stageId](const std::string &body, const auto &span) {
                              return rerenderStage(stageId, body, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/:animationId/rerender",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string animationId(request->getParameter("animationId"));
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/{animationId}/rerender", "rerenderAnimation",
                          "StageController", 4096,
                          [animationId](const std::string &body, const auto &span) {
                              return rerenderAnimation(animationId, body, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .get("/api/v1/storyboard",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/storyboard", "listStoryboards", "StoryboardController",
                         false, [](const auto &span) { return listStoryboards(span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/storyboard/:storyboardId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string storyboardId(request->getParameter("storyboardId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/storyboard/{storyboardId}", "getStoryboard",
                         "StoryboardController", false,
                         [storyboardId](const auto &span) { return getStoryboard(storyboardId, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/storyboard",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/storyboard", "createStoryboard", "StoryboardController",
                          1024 * 1024,
                          [](const std::string &body, const auto &span) { return createStoryboard(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .put("/api/v1/storyboard/:storyboardId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string storyboardId(request->getParameter("storyboardId"));
                     runBodyRoute(
                         response, request, "PUT", "/api/v1/storyboard/{storyboardId}", "updateStoryboard",
                         "StoryboardController", 1024 * 1024,
                         [storyboardId](const std::string &body, const auto &span) {
                             return updateStoryboard(storyboardId, body, span);
                         },
                         requestRegistry, applicationExecutor, dispatcher);
                 })
            .del("/api/v1/storyboard/:storyboardId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string storyboardId(request->getParameter("storyboardId"));
                     runBodylessRoute(
                         response, request, "DELETE", "/api/v1/storyboard/{storyboardId}", "deleteStoryboard",
                         "StoryboardController", false,
                         [storyboardId](const auto &span) { return deleteStoryboard(storyboardId, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/debug/cache-invalidate/creature",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/debug/cache-invalidate/creature", "invalidate_creature",
                         "DebugController", false,
                         [](const auto &span) { return invalidateCache(CacheType::Creature, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/debug/cache-invalidate/animation",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/debug/cache-invalidate/animation", "invalidate_animation",
                         "DebugController", false,
                         [](const auto &span) { return invalidateCache(CacheType::Animation, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/debug/cache-invalidate/playlist",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/debug/cache-invalidate/playlist", "invalidate_playlist",
                         "DebugController", false,
                         [](const auto &span) { return invalidateCache(CacheType::Playlist, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/debug/cache-invalidate/fixture",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/debug/cache-invalidate/fixture", "invalidate_fixture",
                         "DebugController", false,
                         [](const auto &span) { return invalidateCache(CacheType::Fixture, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/debug/cache-invalidate/dialog-script-list",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/debug/cache-invalidate/dialog-script-list",
                         "invalidate_dialog_script_list", "DebugController", false,
                         [](const auto &span) { return invalidateCache(CacheType::DialogScriptList, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/sound",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/sound", "getAllSounds", "SoundController", false,
                         [](const auto &span) { return listSounds(false, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/sound/ad-hoc",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/sound/ad-hoc", "getAdHocSounds", "SoundController", false,
                         [](const auto &span) { return listSounds(true, span); }, requestRegistry, &applicationExecutor,
                         &dispatcher);
                 })
            .post("/api/v1/sound/play",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/sound/play", "playSound", "SoundController",
                          api::MAX_SOUND_CONTROL_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return playSound(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .get("/api/v1/voice/list-available",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/voice/list-available", "getAllVoices", "VoiceController",
                         false, [](const auto &span) { return listVoices(span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/voice/subscription",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/voice/subscription", "getSubscriptionStatus",
                         "VoiceController", false, [](const auto &span) { return voiceSubscription(span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/voice",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/voice", "makeSoundFile", "VoiceController",
                          api::MAX_VOICE_REQUEST_BYTES,
                          [](const std::string &body, const auto &span) { return createVoiceFile(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .get("/api/v1/debug/cache-invalidate/storyboard-list",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/debug/cache-invalidate/storyboard-list",
                         "invalidate_storyboard_list", "DebugController", false,
                         [](const auto &span) { return invalidateCache(CacheType::StoryboardList, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/debug/cache-invalidate/stage-list",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/debug/cache-invalidate/stage-list", "invalidate_stage_list",
                         "DebugController", false,
                         [](const auto &span) { return invalidateCache(CacheType::StageList, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/debug/cache-invalidate/sound-list",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/debug/cache-invalidate/sound-list", "invalidate_sound_list",
                         "DebugController", false,
                         [](const auto &span) { return invalidateCache(CacheType::SoundList, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/debug/cache-invalidate/ad-hoc-animation-list",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/debug/cache-invalidate/ad-hoc-animation-list",
                         "invalidate_adhoc_animation_list", "DebugController", false,
                         [](const auto &span) { return invalidateCache(CacheType::AdHocAnimationList, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/debug/cache-invalidate/ad-hoc-sound-list",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/debug/cache-invalidate/ad-hoc-sound-list",
                         "invalidate_adhoc_sound_list", "DebugController", false,
                         [](const auto &span) { return invalidateCache(CacheType::AdHocSoundList, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/debug/cache/audio/prune",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string dryRun(request->getQuery("dry_run"));
                      runBodylessRoute(
                          response, request, "POST", "/api/v1/debug/cache/audio/prune", "prune_audio_cache",
                          "DebugController", false,
                          [dryRun](const auto &span) { return pruneAudioCache(dryRun, span); }, requestRegistry,
                          &applicationExecutor, &dispatcher);
                  })
            .get("/api/v1/debug/playlist/update",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/debug/playlist/update", "test_playlist_updates",
                         "DebugController", false, [](const auto &span) { return sendDebugPlaylistUpdate(span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation/dialog/script",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/dialog/script", "listDialogScripts",
                         "DialogScriptController", false, [](const auto &span) { return listDialogScripts(span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation/dialog/script/:scriptId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string scriptId(request->getParameter("scriptId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/dialog/script/{scriptId}", "getDialogScript",
                         "DialogScriptController", false,
                         [scriptId](const auto &span) { return getDialogScript(scriptId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/animation/dialog/script",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/dialog/script", "createDialogScript",
                          "DialogScriptController", api::MAX_DIALOG_REQUEST_BYTES,
                          [](const std::string &body, const auto &span) { return createDialogScript(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .put("/api/v1/animation/dialog/script/:scriptId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string scriptId(request->getParameter("scriptId"));
                     runBodyRoute(
                         response, request, "PUT", "/api/v1/animation/dialog/script/{scriptId}", "updateDialogScript",
                         "DialogScriptController", api::MAX_DIALOG_REQUEST_BYTES,
                         [scriptId](const std::string &body, const auto &span) {
                             return updateDialogScript(scriptId, body, span);
                         },
                         requestRegistry, applicationExecutor, dispatcher);
                 })
            .post("/api/v1/animation/dialog/script/validate",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/dialog/script/validate", "validateDialogScript",
                          "DialogScriptController", api::MAX_DIALOG_REQUEST_BYTES,
                          [](const std::string &body, const auto &span) { return validateDialogScript(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .del("/api/v1/animation/dialog/script/:scriptId/music",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string scriptId(request->getParameter("scriptId"));
                     runBodylessRoute(
                         response, request, "DELETE", "/api/v1/animation/dialog/script/{scriptId}/music",
                         "clearDialogBackgroundMusic", "DialogScriptController", false,
                         [scriptId](const auto &span) { return clearDialogMusic(scriptId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .del("/api/v1/animation/dialog/script/:scriptId/voice",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string scriptId(request->getParameter("scriptId"));
                     runBodylessRoute(
                         response, request, "DELETE", "/api/v1/animation/dialog/script/{scriptId}/voice",
                         "clearAcceptedVoice", "DialogScriptController", false,
                         [scriptId](const auto &span) { return clearDialogVoice(scriptId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .del("/api/v1/animation/dialog/script/:scriptId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string scriptId(request->getParameter("scriptId"));
                     runBodylessRoute(
                         response, request, "DELETE", "/api/v1/animation/dialog/script/{scriptId}",
                         "deleteDialogScript", "DialogScriptController", false,
                         [scriptId](const auto &span) { return deleteDialogScript(scriptId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/animation/dialog-stream/start",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/dialog-stream/start", "startDialogStream",
                          "DialogStreamController", api::MAX_STREAMING_AD_HOC_CONTROL_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return startDialogStream(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/dialog-stream/turn",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/dialog-stream/turn", "addDialogStreamTurn",
                          "DialogStreamController", api::MAX_STREAMING_AD_HOC_TEXT_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return addDialogStreamTurn(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/dialog-stream/finish",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/dialog-stream/finish", "finishDialogStream",
                          "DialogStreamController", api::MAX_STREAMING_AD_HOC_CONTROL_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return finishDialogStream(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/ad-hoc-stream/start",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/ad-hoc-stream/start", "startStreamingAdHoc",
                          "StreamingAdHocController", api::MAX_STREAMING_AD_HOC_CONTROL_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return startStreamingAdHoc(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/ad-hoc-stream/text",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/ad-hoc-stream/text", "addStreamingAdHocText",
                          "StreamingAdHocController", api::MAX_STREAMING_AD_HOC_TEXT_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return addStreamingAdHocText(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/ad-hoc-stream/finish",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/ad-hoc-stream/finish", "finishStreamingAdHoc",
                          "StreamingAdHocController", api::MAX_STREAMING_AD_HOC_CONTROL_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return finishStreamingAdHoc(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/interrupt",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/interrupt", "interruptAnimation",
                          "AnimationController", api::MAX_ANIMATION_CONTROL_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return interruptAnimation(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/ad-hoc/play",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/ad-hoc/play", "playPreparedAdHocAnimation",
                          "AnimationController", api::MAX_ANIMATION_CONTROL_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) {
                              return playPreparedAdHocAnimation(body, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .get("/api/v1/animation/ad-hoc-stream/exchanges",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string limit(request->getQuery("limit"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/ad-hoc-stream/exchanges", "listExchanges",
                         "StreamingAdHocController", false,
                         [limit](const auto &span) { return listStreamingAdHocExchanges(limit, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation/ad-hoc-stream/exchange/:sessionId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string sessionId(request->getParameter("sessionId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/ad-hoc-stream/exchange/{sessionId}",
                         "getExchange", "StreamingAdHocController", false,
                         [sessionId](const auto &span) { return getStreamingAdHocExchange(sessionId, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/sound/mp3/:filename",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string filename(request->getParameter("filename"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/sound/mp3/{filename}", "getSoundMp3", "SoundController",
                         false,
                         [filename](const auto &span) {
                             return renderSoundRendition(filename, ws::SoundRenditionFormat::Mp3, span);
                         },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/sound/shareable/:filename",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string filename(request->getParameter("filename"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/sound/shareable/{filename}", "getShareableSound",
                         "SoundController", false,
                         [filename](const auto &span) {
                             return renderSoundRendition(filename, ws::SoundRenditionFormat::OggOpus, span);
                         },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/sound/provenance/:filename",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string filename(request->getParameter("filename"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/sound/provenance/{filename}", "getSoundProvenance",
                         "SoundController", false,
                         [filename](const auto &span) { return getSoundProvenance(filename, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/sound/ad-hoc/:filename",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string filename(request->getParameter("filename"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/sound/ad-hoc/{filename}", "getAdHocSound",
                         "SoundController", false,
                         [filename](const auto &span) { return getAdHocSoundFile(filename, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .head("/api/v1/sound/ad-hoc/:filename",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string filename(request->getParameter("filename"));
                      runBodylessRoute(
                          response, request, "HEAD", "/api/v1/sound/ad-hoc/{filename}", "getAdHocSound",
                          "SoundController", true,
                          [filename](const auto &span) { return getAdHocSoundFile(filename, span); }, requestRegistry,
                          &applicationExecutor, &dispatcher);
                  })
            .get("/api/v1/sound/:filename/metadata",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string filename(request->getParameter("filename"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/sound/{filename}/metadata", "getSoundMetadata",
                         "SoundController", false,
                         [filename](const auto &span) { return getSoundMetadata(filename, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/sound/:filename",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string filename(request->getParameter("filename"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/sound/{filename}", "getSound", "SoundController", false,
                         [filename](const auto &span) { return getSoundFile(filename, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .head("/api/v1/sound/:filename",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string filename(request->getParameter("filename"));
                      runBodylessRoute(
                          response, request, "HEAD", "/api/v1/sound/{filename}", "getSound", "SoundController", true,
                          [filename](const auto &span) { return getSoundFile(filename, span); }, requestRegistry,
                          &applicationExecutor, &dispatcher);
                  })
            .get("/api/v1/animation/ad-hoc-stream/exchange/:sessionId/audio.wav",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string sessionId(request->getParameter("sessionId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.wav",
                         "getExchangeAudioWav", "StreamingAdHocController", false,
                         [sessionId](const auto &span) {
                             return getExchangeAudio(sessionId, ExchangeAudioFormat::Wav, span);
                         },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation/ad-hoc-stream/exchange/:sessionId/audio.mp3",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string sessionId(request->getParameter("sessionId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.mp3",
                         "getExchangeAudioMp3", "StreamingAdHocController", false,
                         [sessionId](const auto &span) {
                             return getExchangeAudio(sessionId, ExchangeAudioFormat::Mp3, span);
                         },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation/ad-hoc-stream/exchange/:sessionId/audio.ogg",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string sessionId(request->getParameter("sessionId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/ad-hoc-stream/exchange/{sessionId}/audio.ogg",
                         "getExchangeAudioOgg", "StreamingAdHocController", false,
                         [sessionId](const auto &span) {
                             return getExchangeAudio(sessionId, ExchangeAudioFormat::OggOpus, span);
                         },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation/dialog/preview/audio/:cache_key/:filename",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string cacheKey(request->getParameter("cache_key"));
                     const std::string filename(request->getParameter("filename"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/dialog/preview/audio/{cache_key}/{filename}",
                         "getPreviewAudio", "DialogPreviewController", false,
                         [cacheKey, filename](const auto &span) {
                             return getDialogPreviewAudio(cacheKey, filename, span);
                         },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation/dialog/preview/share/:cache_key/:filename",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string cacheKey(request->getParameter("cache_key"));
                     const std::string filename(request->getParameter("filename"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/dialog/preview/share/{cache_key}/{filename}",
                         "getPreviewShareable", "DialogPreviewController", false,
                         [cacheKey, filename](const auto &span) {
                             return getDialogPreviewShareable(cacheKey, filename, span);
                         },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation/dialog/music/generated/:filename",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string filename(request->getParameter("filename"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/dialog/music/generated/{filename}",
                         "getGeneratedMusicMp3", "DialogMusicController", false,
                         [filename](const auto &span) { return getGeneratedMusicMp3(filename, true, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/music/generated/:filename",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string filename(request->getParameter("filename"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/music/generated/{filename}", "getMusicCandidateMp3",
                         "MusicController", false,
                         [filename](const auto &span) { return getGeneratedMusicMp3(filename, false, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/music/generate",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/music/generate", "generateMusic", "MusicController",
                          api::MAX_MUSIC_REQUEST_BYTES,
                          [](const std::string &body, const auto &span) { return generateMusic(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/music/plan",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/music/plan", "planMusic", "MusicController",
                          api::MAX_MUSIC_REQUEST_BYTES,
                          [](const std::string &body, const auto &span) { return planMusic(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .get("/api/v1/music",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/music", "listMusicPieces", "MusicController", false,
                         [](const auto &span) { return listMusicPieces(span); }, requestRegistry, &applicationExecutor,
                         &dispatcher);
                 })
            .get("/api/v1/music/generated/:generationId/recipe",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string generationId(request->getParameter("generationId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/music/generated/{generationId}/recipe",
                         "getMusicCandidateRecipe", "MusicController", false,
                         [generationId](const auto &span) { return getMusicCandidateRecipe(generationId, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/music/generated/:generationId/save",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string generationId(request->getParameter("generationId"));
                      runBodyRoute(
                          response, request, "POST", "/api/v1/music/generated/{generationId}/save",
                          "saveMusicCandidate", "MusicController", api::MAX_MUSIC_REQUEST_BYTES,
                          [generationId](const std::string &body, const auto &span) {
                              return saveMusicCandidate(generationId, body, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .get("/api/v1/music/:pieceId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string pieceId(request->getParameter("pieceId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/music/{pieceId}", "getMusicPiece", "MusicController", false,
                         [pieceId](const auto &span) { return getMusicPiece(pieceId, span); }, requestRegistry,
                         &applicationExecutor, &dispatcher);
                 })
            .put("/api/v1/music/:pieceId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string pieceId(request->getParameter("pieceId"));
                     runBodyRoute(
                         response, request, "PUT", "/api/v1/music/{pieceId}", "updateMusicPiece", "MusicController",
                         api::MAX_MUSIC_REQUEST_BYTES,
                         [pieceId](const std::string &body, const auto &span) {
                             return updateMusicPiece(pieceId, body, span);
                         },
                         requestRegistry, applicationExecutor, dispatcher);
                 })
            .del("/api/v1/music/:pieceId",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string pieceId(request->getParameter("pieceId"));
                     runBodylessRoute(
                         response, request, "DELETE", "/api/v1/music/{pieceId}", "deleteMusicPiece", "MusicController",
                         false, [pieceId](const auto &span) { return deleteMusicPiece(pieceId, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/music/:pieceId/refine",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string pieceId(request->getParameter("pieceId"));
                      runBodyRoute(
                          response, request, "POST", "/api/v1/music/{pieceId}/refine", "refineMusicPiece",
                          "MusicController", api::MAX_MUSIC_REQUEST_BYTES,
                          [pieceId](const std::string &body, const auto &span) {
                              return refineMusicPiece(pieceId, body, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/dialog/music",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/dialog/music", "submitDialogMusic",
                          "DialogMusicController", api::MAX_DIALOG_REQUEST_BYTES,
                          [](const std::string &body, const auto &span) { return submitDialogMusic(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/dialog/music/plan",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/dialog/music/plan", "planDialogMusic",
                          "DialogMusicController", api::MAX_DIALOG_REQUEST_BYTES,
                          [](const std::string &body, const auto &span) { return planDialogMusic(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .get("/api/v1/animation/dialog/music/finetunes",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/dialog/music/finetunes", "listMusicFinetunes",
                         "DialogMusicController", false, [](const auto &span) { return listMusicFinetunes(span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .get("/api/v1/animation/dialog/music/generated/:generationId/recipe",
                 [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                     const std::string generationId(request->getParameter("generationId"));
                     runBodylessRoute(
                         response, request, "GET", "/api/v1/animation/dialog/music/generated/{generationId}/recipe",
                         "getGeneratedMusicRecipe", "DialogMusicController", false,
                         [generationId](const auto &span) { return getMusicCandidateRecipe(generationId, span); },
                         requestRegistry, &applicationExecutor, &dispatcher);
                 })
            .post("/api/v1/animation/dialog/music/generated/:generationId/promote",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      const std::string generationId(request->getParameter("generationId"));
                      runBodylessRoute(
                          response, request, "POST", "/api/v1/animation/dialog/music/generated/{generationId}/promote",
                          "promoteGeneratedMusic", "DialogMusicController", false,
                          [generationId](const auto &span) { return promoteGeneratedMusic(generationId, span); },
                          requestRegistry, &applicationExecutor, &dispatcher);
                  })
            .post("/api/v1/animation/dialog",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/dialog", "submitDialog", "DialogController",
                          api::MAX_DIALOG_REQUEST_BYTES,
                          [](const std::string &body, const auto &span) { return submitDialog(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/dialog/voice/accept",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/dialog/voice/accept", "acceptVoiceTake",
                          "DialogVoiceController", api::MAX_DIALOG_REQUEST_BYTES,
                          [](const std::string &body, const auto &span) { return acceptVoiceTake(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/dialog/preview/lookup",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/dialog/preview/lookup", "lookupPreview",
                          "DialogPreviewController", api::MAX_DIALOG_REQUEST_BYTES,
                          [](const std::string &body, const auto &span) { return lookupDialogPreview(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/dialog/preview/meta",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/dialog/preview/meta", "submitPreviewMeta",
                          "DialogPreviewController", api::MAX_DIALOG_REQUEST_BYTES,
                          [](const std::string &body, const auto &span) { return submitDialogPreviewMeta(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/animation/dialog/preview/multichannel",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/animation/dialog/preview/multichannel",
                          "submitPreviewMultichannel", "DialogPreviewController", api::MAX_DIALOG_REQUEST_BYTES,
                          [](const std::string &body, const auto &span) {
                              return submitDialogPreviewMultichannel(body, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/sound/generate-lipsync",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      runBodyRoute(
                          response, request, "POST", "/api/v1/sound/generate-lipsync", "generateLipSync",
                          "SoundController", api::MAX_SOUND_CONTROL_REQUEST_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return generateLipSync(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/sound/generate-lipsync/upload",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      // Raw WAV bytes, not JSON: the transport only bounds and
                      // collects the body; the worker writes it to a temp file.
                      const std::string filename(request->getQuery("filename"));
                      runBodyRoute(
                          response, request, "POST", "/api/v1/sound/generate-lipsync/upload",
                          "generateLipSyncFromUpload", "SoundController", api::MAX_SOUND_UPLOAD_BODY_BYTES,
                          [filename](const std::string &body, const auto &span) {
                              return generateLipSyncFromUpload(filename, body, span);
                          },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .post("/api/v1/stt/transcribe",
                  [&applicationExecutor, &dispatcher, &requestRegistry](auto *response, auto *request) {
                      // Raw float32 PCM, not JSON.
                      runBodyRoute(
                          response, request, "POST", "/api/v1/stt/transcribe", "transcribeAudio",
                          "SpeechToTextController", api::MAX_SPEECH_TO_TEXT_BODY_BYTES,
                          [](const std::string &body, const auto &span) { return transcribeAudio(body, span); },
                          requestRegistry, applicationExecutor, dispatcher);
                  })
            .ws<WebSocketSession>(
                "/api/v1/websocket",
                {.compression = uWS::DISABLED,
                 .maxPayloadLength = WEBSOCKET_INBOUND_MESSAGE_LIMIT,
                 .idleTimeout = 120,
                 .maxBackpressure = WEBSOCKET_BACKPRESSURE_LIMIT,
                 .closeOnBackpressureLimit = true,
                 .resetIdleTimeoutOnSend = false,
                 .sendPingsAutomatically = true,
                 .maxLifetime = 0,
                 .upgrade =
                     [&nextWebSocketConnectionId](auto *response, auto *request, auto *context) {
                         const std::string traceparent(request->getHeader("traceparent"));
                         const auto [triggerTraceId, triggerSpanId] = traceIdsFromTraceparent(traceparent);
                         auto span = createRequestSpan(response, request, "GET", "/api/v1/websocket", "ws",
                                                       "WebSocketController");
                         if (span) {
                             span->setAttribute("transport.outcome", "websocket_upgraded");
                             span->setHttpStatus(101);
                         }

                         WebSocketSession session;
                         session.connectionId = nextWebSocketConnectionId++;
                         session.triggerTraceId = triggerTraceId;
                         session.triggerSpanId = triggerSpanId;
                         response->template upgrade<WebSocketSession>(
                             std::move(session), request->getHeader("sec-websocket-key"),
                             request->getHeader("sec-websocket-protocol"),
                             request->getHeader("sec-websocket-extensions"), context);
                     },
                 .open =
                     [&webSockets, &websocketConnectionCount](CreatureWebSocket *webSocket) {
                         auto *session = webSocket->getUserData();
                         webSockets.insert_or_assign(session->connectionId, webSocket);
                         ++websocketConnectionCount;
                         webSocket->subscribe(WEBSOCKET_BROADCAST_TOPIC);
                         if (creatures::metrics) {
                             creatures::metrics->incrementWebsocketConnectionsProcessed();
                         }
                         spdlog::info("uWebSockets client {} connected from {}", session->connectionId,
                                      webSocket->getRemoteAddressAsText());
                     },
                 .message =
                     [&dispatchWebSocketMessage](CreatureWebSocket *webSocket, const std::string_view message,
                                                 const uWS::OpCode) {
                         auto *session = webSocket->getUserData();
                         if (creatures::metrics) {
                             creatures::metrics->incrementWebsocketMessagesReceived();
                         }
                         if (session->mailbox.size() >= WEBSOCKET_CONNECTION_MAILBOX_LIMIT) {
                             spdlog::warn("Closing WebSocket client {} because its inbound mailbox is full",
                                          session->connectionId);
                             webSocket->end(1013, "Inbound queue full");
                             return;
                         }
                         session->mailbox.push_back(
                             {.payload = std::string(message), .sequence = ++session->nextSequence});
                         dispatchWebSocketMessage(session->connectionId);
                     },
                 .dropped =
                     [](CreatureWebSocket *webSocket, const std::string_view message, const int) {
                         const auto connectionId = webSocket->getUserData()->connectionId;
                         auto span =
                             creatures::observability
                                 ? creatures::observability->createSamplingSpan("WebSocket.publish.dropped", 0.0005)
                                 : nullptr;
                         if (span) {
                             span->setAttribute("transport.framework", "uwebsockets");
                             span->setAttribute("websocket.connection.id", static_cast<int64_t>(connectionId));
                             span->setAttribute("websocket.message.size", static_cast<int64_t>(message.size()));
                             span->setAttribute("error.type", "WebSocketBackpressure");
                             span->setError(
                                 "WebSocket message dropped because the client exceeded backpressure limits");
                         }
                         spdlog::warn("Dropped outbound WebSocket message for backpressured client {}", connectionId);
                     },
                 .pong =
                     [](CreatureWebSocket *webSocket, const std::string_view) {
                         if (creatures::metrics) {
                             creatures::metrics->incrementWebsocketPongsReceived();
                         }
                         spdlog::trace("Received pong from uWebSockets client {}",
                                       webSocket->getUserData()->connectionId);
                     },
                 .close =
                     [&webSockets, &websocketConnectionCount](CreatureWebSocket *webSocket, const int code,
                                                              const std::string_view message) {
                         const auto connectionId = webSocket->getUserData()->connectionId;
                         webSockets.erase(connectionId);
                         if (websocketConnectionCount > 0) {
                             --websocketConnectionCount;
                         }
                         spdlog::info("uWebSockets client {} disconnected (code={}, message={})", connectionId, code,
                                      message);
                     }})
            .any("/*", [&requestRegistry](auto *response, auto *request) {
                const auto method = request->getCaseSensitiveMethod();
                runBodylessRoute(
                    response, request, method, request->getUrl(), "notFound", "UWebSocketsServer", method == "HEAD",
                    [](const auto &) { return statusResponse(404, "Route not found"); }, requestRegistry);
            });

        bool listening = false;
        app.listen("0.0.0.0", configuredPort, [&listening](auto *socket) { listening = socket != nullptr; });
        if (!listening) {
            {
                std::lock_guard lock(lifecycleMutex_);
                loop_ = nullptr;
                app_ = nullptr;
                shutdownAction_ = {};
            }
            publishStartState(StartState::Failed,
                              fmt::format("uWebSockets failed to listen on port {}", configuredPort));
            return;
        }

        websocketBroadcastThread = std::jthread([&dispatcher, &app, &websocketConnectionCount,
                                                 &outboundWebSocketSequence](const std::stop_token stopToken) {
            setThreadName("uWebSockets::broadcast");
            std::string message;
            while (!stopToken.stop_requested()) {
                if (!creatures::websocketOutgoingMessages) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                if (!creatures::websocketOutgoingMessages->wait_dequeue_timed(message,
                                                                              std::chrono::milliseconds(100))) {
                    continue;
                }
                if (stopToken.stop_requested()) {
                    return;
                }
                if (!dispatcher.post([message = std::move(message), &app, &websocketConnectionCount,
                                      &outboundWebSocketSequence]() mutable {
                        const auto subscriberCount = websocketConnectionCount;
                        const auto sequence = ++outboundWebSocketSequence;
                        auto span = creatures::observability
                                        ? creatures::observability->createSamplingSpan("WebSocket.publish", 0.0005)
                                        : nullptr;
                        if (span) {
                            span->setAttribute("transport.framework", "uwebsockets");
                            span->setAttribute("websocket.message.sequence", static_cast<int64_t>(sequence));
                            span->setAttribute("websocket.message.size", static_cast<int64_t>(message.size()));
                            span->setAttribute("websocket.subscriber.count", static_cast<int64_t>(subscriberCount));
                        }

                        const bool published =
                            app.publish(WEBSOCKET_BROADCAST_TOPIC, message, uWS::OpCode::TEXT, false);
                        if (published && creatures::metrics) {
                            for (std::size_t index = 0; index < subscriberCount; ++index) {
                                creatures::metrics->incrementWebsocketMessagesSent();
                            }
                        }
                        if (span) {
                            span->setAttribute("websocket.broadcast.outcome",
                                               subscriberCount == 0 ? "no_subscribers" : "published");
                            span->setAttribute("websocket.broadcast.accepted", published);
                            span->setSuccess();
                        }
                    })) {
                    return;
                }
            }
        });

        publishStartState(StartState::Running);
        spdlog::info("uWebSockets migration transport listening on 0.0.0.0:{}", configuredPort);
        app.run();

        // This also covers an unexpected loop exit that did not pass through
        // shutdownAction_: cancel loop-owned requests before joining workers.
        applicationExecutor.requestStop();
        fileStreamer.requestStop();
        auto cancelled = requestRegistry.cancelAll("shutdown_cancelled", "Transport loop stopped", 503);
        for (auto &request : cancelled) {
            if (request.response != nullptr) {
                static_cast<uWS::HttpResponse<false> *>(request.response)->close();
            }
        }
        dispatcher.close();
        websocketBroadcastThread.request_stop();
        if (websocketBroadcastThread.joinable()) {
            websocketBroadcastThread.join();
        }
        applicationExecutor.join();
        fileStreamer.join();

        {
            std::lock_guard lock(lifecycleMutex_);
            loop_ = nullptr;
            app_ = nullptr;
            shutdownAction_ = {};
            startState_ = StartState::Stopped;
        }
        lifecycleCondition_.notify_all();
        spdlog::info("uWebSockets migration transport stopped");
    } catch (const std::exception &error) {
        {
            std::lock_guard lock(lifecycleMutex_);
            loop_ = nullptr;
            app_ = nullptr;
            shutdownAction_ = {};
        }
        publishStartState(StartState::Failed, fmt::format("uWebSockets transport failed: {}", error.what()));
    } catch (...) {
        {
            std::lock_guard lock(lifecycleMutex_);
            loop_ = nullptr;
            app_ = nullptr;
            shutdownAction_ = {};
        }
        publishStartState(StartState::Failed, "uWebSockets transport failed with an unknown exception");
    }
}

} // namespace creatures::transport
