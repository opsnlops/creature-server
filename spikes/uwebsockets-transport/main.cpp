#include <App.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef CREATURE_UWS_OTEL_GATE
#include <mongocxx/instance.hpp>

#include "model/DmxFixture.h"
#include "server/database.h"
#include "server/ws/service/DmxFixtureService.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<ObjectCache<fixtureId_t, DmxFixture>> fixtureCache;
} // namespace creatures
#endif

namespace {

constexpr std::uint16_t DEFAULT_SERVER_PORT = 8000;
constexpr std::size_t FIXTURE_BODY_LIMIT = 1024 * 1024;
constexpr unsigned int WEBSOCKET_MESSAGE_LIMIT = 64 * 1024;
constexpr std::size_t FILE_CHUNK_SIZE = 64 * 1024;
constexpr std::size_t BLOCKING_WORKER_COUNT = 4;
constexpr std::size_t BLOCKING_QUEUE_LIMIT = 8;
constexpr std::size_t FILE_WORKER_COUNT = 2;
constexpr std::size_t FILE_QUEUE_LIMIT = 8;
constexpr std::string_view JSON_CONTENT_TYPE = "application/json; charset=utf-8";
constexpr std::string_view HEALTH_BODY = R"({"status":"ok","code":200,"message":"Server is operational"})";
constexpr std::string_view BROADCAST_TOPIC = "creature-server-broadcast";

class BlockingExecutor {
  public:
    using Task = std::function<void(std::stop_token)>;

    BlockingExecutor(std::size_t workerCount, std::size_t queueLimit) : queueLimit_(queueLimit) {
        workers_.reserve(workerCount);
        for (std::size_t index = 0; index < workerCount; ++index) {
            workers_.emplace_back([this](std::stop_token stopToken) { run(stopToken); });
        }
    }

    ~BlockingExecutor() {
        requestStop();
        join();
    }

    BlockingExecutor(const BlockingExecutor &) = delete;
    BlockingExecutor &operator=(const BlockingExecutor &) = delete;

    bool trySubmit(Task task) {
        std::lock_guard lock(mutex_);
        if (stopping_ || queue_.size() >= queueLimit_) {
            return false;
        }
        queue_.push_back(std::move(task));
        condition_.notify_one();
        return true;
    }

    void requestStop() {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            queue_.clear();
        }
        for (auto &worker : workers_) {
            worker.request_stop();
        }
        condition_.notify_all();
    }

    void join() {
        for (auto &worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

  private:
    void run(std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            Task task;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(
                    lock, [this, &stopToken] { return stopping_ || stopToken.stop_requested() || !queue_.empty(); });
                if (stopping_ || stopToken.stop_requested()) {
                    return;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                task(stopToken);
            } catch (...) {
                // A transport worker must not terminate the process. Request-specific
                // tasks provide their own typed error response where possible.
            }
        }
    }

    const std::size_t queueLimit_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Task> queue_;
    std::vector<std::jthread> workers_;
    bool stopping_ = false;
};

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

    void closeAndPost(std::function<void()> callback) {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            return;
        }
        accepting_ = false;
        loop_->defer(std::move(callback));
    }

  private:
    uWS::Loop *loop_;
    std::mutex mutex_;
    bool accepting_ = true;
};

class FixtureStore {
  public:
    nlohmann::json upsert(nlohmann::json fixture) {
        std::lock_guard lock(mutex_);
        const auto id = fixture.at("id").get<std::string>();
        fixtures_.insert_or_assign(id, fixture);
        return fixture;
    }

    std::optional<nlohmann::json> get(const std::string &id) const {
        std::lock_guard lock(mutex_);
        const auto found = fixtures_.find(id);
        return found == fixtures_.end() ? std::nullopt : std::optional<nlohmann::json>{found->second};
    }

    bool erase(const std::string &id) {
        std::lock_guard lock(mutex_);
        return fixtures_.erase(id) != 0;
    }

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, nlohmann::json> fixtures_;
};

struct BodyState {
    std::string body;
    bool responded = false;
    bool aborted = false;
    bool oversized = false;
    bool chunked = false;
};

class FileDescriptor {
  public:
    explicit FileDescriptor(int descriptor) : descriptor_(descriptor) {}
    ~FileDescriptor() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    FileDescriptor &operator=(FileDescriptor &&) = delete;

    [[nodiscard]] int get() const { return descriptor_; }

  private:
    int descriptor_;
};

struct FileState {
    uWS::HttpResponse<false> *response;
    std::shared_ptr<FileDescriptor> file;
    std::uintmax_t size = 0;
    unsigned int injectedReadDelayMilliseconds = 0;
    bool aborted = false;
    bool readInFlight = false;
};

std::optional<std::uint16_t> parsePort(std::string_view value) {
    unsigned int parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 || parsed > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(parsed);
}

std::uint16_t serverPort() {
    const char *configured = std::getenv("SERVER_PORT");
    if (configured == nullptr) {
        return DEFAULT_SERVER_PORT;
    }
    const auto parsed = parsePort(configured);
    if (!parsed.has_value()) {
        throw std::runtime_error("SERVER_PORT must be an integer between 1 and 65535");
    }
    return parsed.value();
}

unsigned int injectedFileReadDelayMilliseconds() {
    const char *configured = std::getenv("SPIKE_FILE_READ_DELAY_MS");
    if (configured == nullptr) {
        return 0;
    }
    unsigned int parsed = 0;
    const std::string_view value(configured);
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed > 1000) {
        throw std::runtime_error("SPIKE_FILE_READ_DELAY_MS must be between 0 and 1000");
    }
    return parsed;
}

#ifdef CREATURE_UWS_OTEL_GATE
std::string mongoUri() {
    const char *configured = std::getenv("SPIKE_MONGODB_URI");
    if (configured == nullptr || std::string_view(configured).empty()) {
        throw std::runtime_error("SPIKE_MONGODB_URI is required for the OTel/Mongo gate");
    }
    return configured;
}

int fixtureErrorStatus(creatures::ServerError::Code code) {
    if (code == creatures::ServerError::NotFound) {
        return 404;
    }
    if (code == creatures::ServerError::InvalidData) {
        return 400;
    }
    return 500;
}
#endif

std::filesystem::path soundsLocation(int argc, char **argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) != "--sounds-location") {
            continue;
        }
        if (index + 1 >= argc) {
            throw std::runtime_error("--sounds-location requires a path");
        }
        return std::filesystem::weakly_canonical(argv[index + 1]);
    }
    throw std::runtime_error("--sounds-location is required for the µWebSockets spike");
}

template <bool SSL>
void sendJson(uWS::HttpResponse<SSL> *response, std::string_view status, std::string body,
              bool closeConnection = false) {
    response->writeStatus(status)->writeHeader("Content-Type", JSON_CONTENT_TYPE)->end(body, closeConnection);
}

template <bool SSL>
void sendStatus(uWS::HttpResponse<SSL> *response, int code, std::string message, bool closeConnection = false) {
    const std::string statusName = code == 404 ? "not_found" : (code < 400 ? "ok" : "error");
    const std::string statusLine = std::to_string(code) + (code == 202   ? " Accepted"
                                                           : code == 400 ? " Bad Request"
                                                           : code == 404 ? " Not Found"
                                                           : code == 413 ? " Payload Too Large"
                                                           : code == 503 ? " Service Unavailable"
                                                                         : " OK");
    sendJson(response, statusLine,
             nlohmann::json{{"status", statusName}, {"code", code}, {"message", std::move(message)}}.dump(),
             closeConnection);
}

template <bool SSL> void sendFixtureValidationError(uWS::HttpResponse<SSL> *response, std::string message) {
    sendJson(response, "200 OK",
             nlohmann::json{{"valid", false},
                            {"missing_creature_ids", nlohmann::json::array()},
                            {"error_messages", {std::move(message)}}}
                 .dump());
}

bool isUuid(std::string_view value) {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        if (separator ? value[index] != '-' : std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
            return false;
        }
    }
    return true;
}

#ifndef CREATURE_UWS_OTEL_GATE
std::optional<unsigned int> parseDelay(std::string_view value) {
    unsigned int parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed > 10000) {
        return std::nullopt;
    }
    return parsed;
}
#endif

bool interruptibleDelay(std::stop_token stopToken, unsigned int milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !stopToken.stop_requested();
}

struct DeferredResponseState {
    uWS::HttpResponse<false> *response;
    bool aborted = false;
#ifdef CREATURE_UWS_OTEL_GATE
    std::shared_ptr<creatures::RequestSpan> requestSpan;
#endif
};

struct PreparedHttpResponse {
    int statusCode;
    std::string status;
    std::string body;
#ifdef CREATURE_UWS_OTEL_GATE
    std::string transportOutcome = "response_completed";
    std::string errorType;
    std::string errorCode;
    std::string errorMessage;
#endif
};

#ifdef CREATURE_UWS_OTEL_GATE
struct WebSocketState {
    std::shared_ptr<creatures::RequestSpan> upgradeSpan;
    std::shared_ptr<creatures::SamplingSpan> sessionSpan;
    bool failed = false;
};

class GateGlobalLifetime {
  public:
    ~GateGlobalLifetime() {
        // mongocxx::instance is declared before this guard in main, so it
        // remains alive while every global object backed by the driver tears
        // down, including on an exception path.
        creatures::db.reset();
        creatures::fixtureCache.reset();
        creatures::observability.reset();
    }
};

class AsyncTraceCompletion {
  public:
    explicit AsyncTraceCompletion(std::shared_ptr<creatures::OperationSpan> span) : span_(std::move(span)) {}

    ~AsyncTraceCompletion() {
        if (!finished_ && span_) {
            span_->setAttribute("transport.outcome", "abandoned");
            span_->setError("Async operation abandoned before loop completion");
            span_->end();
        }
    }

    void success(std::string_view outcome) {
        if (finished_ || !span_) {
            return;
        }
        span_->setAttribute("transport.outcome", std::string(outcome));
        span_->setSuccess();
        span_->end();
        finished_ = true;
    }

    void fail(std::string_view outcome, std::string_view message) {
        if (finished_ || !span_) {
            return;
        }
        span_->setAttribute("transport.outcome", std::string(outcome));
        span_->setError(std::string(message));
        span_->end();
        finished_ = true;
    }

    [[nodiscard]] const std::shared_ptr<creatures::OperationSpan> &span() const { return span_; }

  private:
    std::shared_ptr<creatures::OperationSpan> span_;
    bool finished_ = false;
};
#else
using WebSocketState = int;
#endif

std::string statusLine(int code) {
    return std::to_string(code) + (code == 202   ? " Accepted"
                                   : code == 400 ? " Bad Request"
                                   : code == 404 ? " Not Found"
                                   : code == 413 ? " Payload Too Large"
                                   : code == 500 ? " Internal Server Error"
                                   : code == 503 ? " Service Unavailable"
                                                 : " OK");
}

PreparedHttpResponse preparedStatus(int code, std::string message) {
    const std::string statusName = code == 404 ? "not_found" : (code < 400 ? "ok" : "error");
    auto prepared = PreparedHttpResponse{
        .statusCode = code,
        .status = statusLine(code),
        .body = nlohmann::json{{"status", statusName}, {"code", code}, {"message", message}}.dump()};
#ifdef CREATURE_UWS_OTEL_GATE
    if (code >= 400) {
        prepared.errorType = "HttpError";
        prepared.errorCode = std::to_string(code);
        prepared.errorMessage = std::move(message);
    }
#endif
    return prepared;
}

template <typename Work>
void dispatchPreparedResponse(uWS::HttpResponse<false> *response, BlockingExecutor &executor,
                              LoopDispatcher &dispatcher, Work work
#ifdef CREATURE_UWS_OTEL_GATE
                              ,
                              std::shared_ptr<creatures::RequestSpan> requestSpan = nullptr
#endif
) {
    auto state = std::make_shared<DeferredResponseState>(DeferredResponseState{
        .response = response,
#ifdef CREATURE_UWS_OTEL_GATE
        .requestSpan = std::move(requestSpan),
#endif
    });
    response->onAborted([state] {
        state->aborted = true;
        state->response = nullptr;
#ifdef CREATURE_UWS_OTEL_GATE
        if (state->requestSpan) {
            state->requestSpan->setAttribute("transport.outcome", "client_aborted");
            state->requestSpan->setError("Client aborted before the response completed");
            state->requestSpan->setHttpStatus(499);
        }
#endif
    });

    const bool accepted =
        executor.trySubmit([state, &dispatcher, work = std::move(work)](std::stop_token stopToken) mutable {
            const auto closeOnFailure = [&state, &dispatcher]() noexcept {
                try {
                    static_cast<void>(dispatcher.post([state] {
                        if (state->aborted || state->response == nullptr) {
                            return;
                        }
                        auto *failedResponse = state->response;
                        state->response = nullptr;
#ifdef CREATURE_UWS_OTEL_GATE
                        if (state->requestSpan) {
                            state->requestSpan->setAttribute("transport.outcome", "defer_failed");
                            state->requestSpan->setError("Unable to return response to the transport loop");
                            state->requestSpan->setHttpStatus(500);
                        }
#endif
                        failedResponse->close();
                    }));
                } catch (...) {
                }
            };
            try {
                std::optional<PreparedHttpResponse> prepared;
                try {
                    prepared = work(stopToken);
                } catch (const std::exception &error) {
                    prepared = preparedStatus(500, "Blocking work failed");
#ifdef CREATURE_UWS_OTEL_GATE
                    prepared->errorType = "BlockingWorkException";
                    prepared->errorCode = "blocking_work_failed";
                    prepared->errorMessage = error.what();
#endif
                } catch (...) {
                    prepared = preparedStatus(500, "Blocking work failed");
#ifdef CREATURE_UWS_OTEL_GATE
                    prepared->errorType = "UnknownBlockingWorkException";
                    prepared->errorCode = "blocking_work_failed";
#endif
                }
                if (!prepared.has_value()) {
                    return;
                }
                if (!dispatcher.post([state, prepared = std::move(prepared.value())] {
                        if (state->aborted || state->response == nullptr) {
                            return;
                        }
                        auto *completedResponse = state->response;
                        state->response = nullptr;
#ifdef CREATURE_UWS_OTEL_GATE
                        if (state->requestSpan) {
                            state->requestSpan->setAttribute("transport.outcome", prepared.transportOutcome);
                            state->requestSpan->setAttribute("http.response.body.size",
                                                             static_cast<int64_t>(prepared.body.size()));
                            if (!prepared.errorType.empty()) {
                                state->requestSpan->setAttribute("error.type", prepared.errorType);
                                state->requestSpan->setAttribute("error.code", prepared.errorCode);
                                state->requestSpan->setError(prepared.errorMessage);
                            }
                            state->requestSpan->setHttpStatus(prepared.statusCode);
                        }
#endif
                        sendJson(completedResponse, prepared.status, std::move(prepared.body));
                    })) {
                    closeOnFailure();
                }
            } catch (...) {
                closeOnFailure();
            }
        });
    if (!accepted) {
        state->response = nullptr;
#ifdef CREATURE_UWS_OTEL_GATE
        if (state->requestSpan) {
            state->requestSpan->setAttribute("transport.outcome", "queue_saturated");
            state->requestSpan->setAttribute("error.type", "QueueSaturated");
            state->requestSpan->setAttribute("error.code", "blocking_queue_full");
            state->requestSpan->setError("Blocking work queue is full");
            state->requestSpan->setHttpStatus(503);
        }
#endif
        sendStatus(response, 503, "Blocking work queue is full");
    }
}

#ifndef CREATURE_UWS_OTEL_GATE
void dispatchBlockingResponse(uWS::HttpResponse<false> *response, BlockingExecutor &executor,
                              LoopDispatcher &dispatcher, unsigned int delayMilliseconds) {
    dispatchPreparedResponse(response, executor, dispatcher,
                             [delayMilliseconds](std::stop_token stopToken) -> std::optional<PreparedHttpResponse> {
                                 if (!interruptibleDelay(stopToken, delayMilliseconds)) {
                                     return std::nullopt;
                                 }
                                 return preparedStatus(200, "Blocking work completed");
                             });
}
#endif

struct SoundFileResult {
    std::shared_ptr<FileDescriptor> file;
    std::uintmax_t size = 0;
    int errorStatus = 0;
};

SoundFileResult openSoundFile(const std::shared_ptr<FileDescriptor> &soundsDirectory, std::string_view requestedName) {
    if (requestedName.empty() || requestedName == "." || requestedName == ".." ||
        requestedName.find('/') != std::string_view::npos || requestedName.find('\\') != std::string_view::npos ||
        requestedName.find('\0') != std::string_view::npos) {
        return {.file = {}, .size = 0, .errorStatus = 404};
    }

    const std::string name(requestedName);
    const int descriptor =
        ::openat(soundsDirectory->get(), name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        return {.file = {}, .size = 0, .errorStatus = 404};
    }
    FileDescriptor ownedDescriptor(descriptor);
    struct stat metadata{};
    if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
        return {.file = {}, .size = 0, .errorStatus = 404};
    }
    return {.file = std::make_shared<FileDescriptor>(std::move(ownedDescriptor)),
            .size = static_cast<std::uintmax_t>(metadata.st_size)};
}

void closeFileResponseOnLoop(const std::shared_ptr<FileState> &state, LoopDispatcher &dispatcher) noexcept {
    try {
        static_cast<void>(dispatcher.post([state] {
            state->readInFlight = false;
            if (state->aborted || state->response == nullptr) {
                return;
            }
            auto *failedResponse = state->response;
            state->response = nullptr;
            failedResponse->close();
        }));
    } catch (...) {
    }
}

void scheduleFileRead(const std::shared_ptr<FileState> &state, BlockingExecutor &fileExecutor,
                      LoopDispatcher &dispatcher) {
    if (state->aborted || state->response == nullptr || state->readInFlight) {
        return;
    }
    const auto offset = state->response->getWriteOffset();
    if (offset >= state->size) {
        return;
    }
    const auto requested = static_cast<std::size_t>(std::min<std::uintmax_t>(state->size - offset, FILE_CHUNK_SIZE));
    state->readInFlight = true;
    const bool accepted =
        fileExecutor.trySubmit([state, offset, requested, &fileExecutor, &dispatcher](std::stop_token stopToken) {
            try {
                if (!interruptibleDelay(stopToken, state->injectedReadDelayMilliseconds)) {
                    return;
                }
                auto bytes = std::make_shared<std::string>(requested, '\0');
                const auto received = ::pread(state->file->get(), bytes->data(), requested, static_cast<off_t>(offset));
                if (stopToken.stop_requested()) {
                    return;
                }
                if (!dispatcher.post([state, offset, received, bytes = std::move(bytes), &fileExecutor, &dispatcher] {
                        state->readInFlight = false;
                        if (state->aborted || state->response == nullptr) {
                            return;
                        }
                        if (received <= 0 || state->response->getWriteOffset() != offset) {
                            auto *failedResponse = state->response;
                            state->response = nullptr;
                            failedResponse->close();
                            return;
                        }
                        bytes->resize(static_cast<std::size_t>(received));
                        const auto [writeAccepted, completed] = state->response->tryEnd(*bytes, state->size);
                        if (completed) {
                            state->response = nullptr;
                            return;
                        }
                        if (writeAccepted) {
                            scheduleFileRead(state, fileExecutor, dispatcher);
                        }
                    })) {
                    closeFileResponseOnLoop(state, dispatcher);
                }
            } catch (...) {
                closeFileResponseOnLoop(state, dispatcher);
            }
        });
    if (!accepted) {
        auto *failedResponse = state->response;
        state->response = nullptr;
        state->readInFlight = false;
        failedResponse->close();
    }
}

void serveSound(uWS::HttpResponse<false> *response, const std::shared_ptr<FileDescriptor> &soundsDirectory,
                std::string requestedName, bool headOnly, BlockingExecutor &fileExecutor, LoopDispatcher &dispatcher,
                unsigned int readDelayMilliseconds) {
    auto state = std::make_shared<FileState>(FileState{.response = response,
                                                       .file = {},
                                                       .size = 0,
                                                       .injectedReadDelayMilliseconds = 0,
                                                       .aborted = false,
                                                       .readInFlight = false});
    response->onAborted([state] {
        state->aborted = true;
        state->response = nullptr;
    });

    const bool accepted =
        fileExecutor.trySubmit([state, soundsDirectory, requestedName = std::move(requestedName), headOnly,
                                &fileExecutor, &dispatcher, readDelayMilliseconds](std::stop_token stopToken) {
            try {
                SoundFileResult opened;
                try {
                    opened = openSoundFile(soundsDirectory, requestedName);
                } catch (...) {
                    opened.errorStatus = 500;
                }
                if (stopToken.stop_requested()) {
                    return;
                }
                if (!dispatcher.post([state, opened = std::move(opened), headOnly, &fileExecutor, &dispatcher,
                                      readDelayMilliseconds]() mutable {
                        if (state->aborted || state->response == nullptr) {
                            return;
                        }
                        if (opened.errorStatus != 0) {
                            auto *failedResponse = state->response;
                            state->response = nullptr;
                            sendStatus(failedResponse, opened.errorStatus,
                                       opened.errorStatus == 404 ? "Sound not found" : "Unable to open sound");
                            return;
                        }
                        if (headOnly) {
                            auto *completedResponse = state->response;
                            state->response = nullptr;
                            completedResponse->writeHeader("Content-Type", "application/octet-stream")
                                ->endWithoutBody(opened.size);
                            return;
                        }
                        state->file = std::move(opened.file);
                        state->size = opened.size;
                        state->injectedReadDelayMilliseconds = readDelayMilliseconds;
                        state->response->writeHeader("Content-Type", "application/octet-stream");
                        state->response->onWritable([state, &fileExecutor, &dispatcher](std::uintmax_t) {
                            if (state->aborted || state->response == nullptr) {
                                return true;
                            }
                            scheduleFileRead(state, fileExecutor, dispatcher);
                            return false;
                        });
                        scheduleFileRead(state, fileExecutor, dispatcher);
                    })) {
                    closeFileResponseOnLoop(state, dispatcher);
                }
            } catch (...) {
                closeFileResponseOnLoop(state, dispatcher);
            }
        });
    if (!accepted) {
        state->response = nullptr;
        sendStatus(response, 503, "File work queue is full");
    }
}

template <bool SSL, typename Completion>
void collectBody(uWS::HttpResponse<SSL> *response, uWS::HttpRequest *request, Completion completion) {
    const auto contentLength = request->getHeader("content-length");
    std::size_t advertisedLength = 0;
    const auto [lengthEnd, lengthError] =
        std::from_chars(contentLength.data(), contentLength.data() + contentLength.size(), advertisedLength);
    const bool hasContentLength = !contentLength.empty() && lengthError == std::errc{} &&
                                  lengthEnd == contentLength.data() + contentLength.size();
    if (hasContentLength && advertisedLength > FIXTURE_BODY_LIMIT) {
        sendStatus(response, 413, "Request body is too large", true);
        return;
    }
    auto state = std::make_shared<BodyState>();
    if (hasContentLength) {
        state->body.reserve(std::min(advertisedLength, FIXTURE_BODY_LIMIT));
    }
    state->chunked = !request->getHeader("transfer-encoding").empty();
    response->onAborted([state] { state->aborted = true; });
    response->onDataV2(
        [response, state, completion = std::move(completion)](std::string_view chunk, std::uint64_t remaining) mutable {
            if (state->aborted || state->responded) {
                return;
            }
            if (state->oversized) {
                if (state->chunked || remaining == 0) {
                    state->responded = true;
                    sendStatus(response, 413, "Request body is too large", state->chunked);
                }
                return;
            }
            if (chunk.size() > FIXTURE_BODY_LIMIT - state->body.size()) {
                state->oversized = true;
                if (state->chunked || remaining == 0) {
                    state->responded = true;
                    sendStatus(response, 413, "Request body is too large", state->chunked);
                }
                return;
            }
            state->body.append(chunk);
            if (remaining == 0) {
                state->responded = true;
                completion(response, std::move(state->body));
            }
        });

    if ((hasContentLength && advertisedLength == 0) ||
        (!hasContentLength && request->getHeader("transfer-encoding").empty())) {
        state->responded = true;
        completion(response, {});
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        const auto port = serverPort();
        const auto soundsRoot = soundsLocation(argc, argv);
        const auto fileReadDelayMilliseconds = injectedFileReadDelayMilliseconds();

#ifdef CREATURE_UWS_OTEL_GATE
        mongocxx::instance mongoInstance;
        GateGlobalLifetime gateGlobalLifetime;
        creatures::observability = std::make_shared<creatures::ObservabilityManager>();
        creatures::observability->initialize("creature-server-uwebsockets-gate", "1.0.0");
        creatures::fixtureCache = std::make_shared<creatures::ObjectCache<fixtureId_t, creatures::DmxFixture>>();
        creatures::db = std::make_shared<creatures::Database>(mongoUri());
        creatures::db->performHealthCheck();
#endif

        sigset_t shutdownSignals;
        sigemptyset(&shutdownSignals);
        sigaddset(&shutdownSignals, SIGINT);
        sigaddset(&shutdownSignals, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &shutdownSignals, nullptr) != 0) {
            throw std::runtime_error("failed to block shutdown signals");
        }

        BlockingExecutor blockingExecutor(BLOCKING_WORKER_COUNT, BLOCKING_QUEUE_LIMIT);
        BlockingExecutor fileExecutor(FILE_WORKER_COUNT, FILE_QUEUE_LIMIT);
        const int soundsDescriptor = ::open(soundsRoot.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
        if (soundsDescriptor < 0) {
            throw std::runtime_error("failed to open sounds directory");
        }
        const auto soundsDirectory = std::make_shared<FileDescriptor>(soundsDescriptor);
#ifndef CREATURE_UWS_OTEL_GATE
        const auto fixtures = std::make_shared<FixtureStore>();
#endif
        uWS::App app;
        LoopDispatcher dispatcher(uWS::Loop::get());

#ifdef CREATURE_UWS_OTEL_GATE
        auto getFixtureHandler = [&blockingExecutor, &dispatcher](auto *response, auto *request) {
            const std::string fixtureId(request->getParameter("fixtureId"));
            const std::string traceparent(request->getHeader("traceparent"));
            auto requestSpan = creatures::observability->createRequestSpan("GET /api/v1/fixture/{fixtureId}", "GET",
                                                                           "/api/v1/fixture/{fixtureId}", traceparent);
            if (requestSpan) {
                requestSpan->setAttribute("http.route", "/api/v1/fixture/{fixtureId}");
                requestSpan->setAttribute("fixture.id", fixtureId);
                requestSpan->setAttribute("transport.framework", "uwebsockets");
                requestSpan->setAttribute("transport.queue.name", "blocking");
            }
            if (!isUuid(fixtureId)) {
                if (requestSpan) {
                    requestSpan->setAttribute("transport.outcome", "validation_rejected");
                    requestSpan->setError("fixtureId must be an RFC 4122 UUID");
                    requestSpan->setHttpStatus(400);
                }
                sendStatus(response, 400, "fixtureId must be an RFC 4122 UUID");
                return;
            }
            dispatchPreparedResponse(
                response, blockingExecutor, dispatcher,
                [fixtureId, requestSpan](std::stop_token stopToken) -> std::optional<PreparedHttpResponse> {
                    if (stopToken.stop_requested()) {
                        auto cancelled = preparedStatus(503, "Request cancelled during shutdown");
                        cancelled.transportOutcome = "shutdown_cancelled";
                        cancelled.errorType = "ShutdownCancellation";
                        cancelled.errorCode = "shutdown_cancelled";
                        return cancelled;
                    }
                    auto result = creatures::ws::DmxFixtureService::getFixture(fixtureId, requestSpan);
                    if (!result.isSuccess()) {
                        const auto error = result.getError().value();
                        return preparedStatus(fixtureErrorStatus(error.getCode()), error.getMessage());
                    }
                    return PreparedHttpResponse{.statusCode = 200,
                                                .status = "200 OK",
                                                .body = creatures::dmxFixtureToJson(result.getValue().value()).dump()};
                },
                requestSpan);
        };
#else
        auto getFixtureHandler = [fixtures, &blockingExecutor, &dispatcher](auto *response, auto *request) {
            const std::string fixtureId(request->getParameter("fixtureId"));
            if (!isUuid(fixtureId)) {
                sendStatus(response, 400, "fixtureId must be an RFC 4122 UUID");
                return;
            }
            dispatchPreparedResponse(response, blockingExecutor, dispatcher,
                                     [fixtures, fixtureId](std::stop_token) -> std::optional<PreparedHttpResponse> {
                                         const auto fixture = fixtures->get(fixtureId);
                                         if (!fixture.has_value()) {
                                             return preparedStatus(404, "Fixture not found");
                                         }
                                         return PreparedHttpResponse{
                                             .statusCode = 200, .status = "200 OK", .body = fixture->dump()};
                                     });
        };
#endif

        app.get("/api/v1/health",
                [](auto *response, auto *) {
                    response->writeHeader("Content-Type", JSON_CONTENT_TYPE)->end(HEALTH_BODY);
                })
            .head("/api/v1/health",
                  [](auto *response, auto *) {
                      response->writeHeader("Content-Type", JSON_CONTENT_TYPE)->endWithoutBody(HEALTH_BODY.size());
                  })
            .get("/api/v1/sound/:soundName",
                 [soundsDirectory, &fileExecutor, &dispatcher, fileReadDelayMilliseconds](auto *response,
                                                                                          auto *request) {
                     serveSound(response, soundsDirectory, std::string(request->getParameter("soundName")), false,
                                fileExecutor, dispatcher, fileReadDelayMilliseconds);
                 })
            .head("/api/v1/sound/:soundName",
                  [soundsDirectory, &fileExecutor, &dispatcher, fileReadDelayMilliseconds](auto *response,
                                                                                           auto *request) {
                      serveSound(response, soundsDirectory, std::string(request->getParameter("soundName")), true,
                                 fileExecutor, dispatcher, fileReadDelayMilliseconds);
                  })
#ifndef CREATURE_UWS_OTEL_GATE
            .post("/api/v1/fixture/validate",
                  [&blockingExecutor, &dispatcher](auto *response, auto *request) {
                      collectBody(
                          response, request,
                          [&blockingExecutor, &dispatcher](auto *completedResponse, std::string body) {
                              dispatchPreparedResponse(
                                  completedResponse, blockingExecutor, dispatcher,
                                  [body = std::move(body)](std::stop_token) -> std::optional<PreparedHttpResponse> {
                                      std::string message = "Fixture validation is not connected in this spike slice";
                                      try {
                                          static_cast<void>(nlohmann::json::parse(body));
                                      } catch (const nlohmann::json::exception &error) {
                                          message = std::string("Invalid fixture JSON: ") + error.what();
                                      }
                                      return PreparedHttpResponse{
                                          .statusCode = 200,
                                          .status = "200 OK",
                                          .body = nlohmann::json{{"valid", false},
                                                                 {"missing_creature_ids", nlohmann::json::array()},
                                                                 {"error_messages", {std::move(message)}}}
                                                      .dump()};
                                  });
                          });
                  })
            .post("/api/v1/fixture",
                  [fixtures, &blockingExecutor, &dispatcher](auto *response, auto *request) {
                      collectBody(
                          response, request,
                          [fixtures, &blockingExecutor, &dispatcher](auto *completedResponse, std::string body) {
                              dispatchPreparedResponse(
                                  completedResponse, blockingExecutor, dispatcher,
                                  [fixtures,
                                   body = std::move(body)](std::stop_token) -> std::optional<PreparedHttpResponse> {
                                      try {
                                          auto fixture = nlohmann::json::parse(body);
                                          if (!fixture.is_object() || !fixture.contains("id") ||
                                              !fixture.at("id").is_string() ||
                                              !isUuid(fixture.at("id").get_ref<const std::string &>())) {
                                              return preparedStatus(400, "Fixture id must be an RFC 4122 UUID");
                                          }
                                          return PreparedHttpResponse{.statusCode = 200,
                                                                      .status = "200 OK",
                                                                      .body =
                                                                          fixtures->upsert(std::move(fixture)).dump()};
                                      } catch (const nlohmann::json::exception &) {
                                          return preparedStatus(400, "Malformed fixture JSON");
                                      }
                                  });
                          });
                  })
#endif
            .get("/api/v1/fixture/:fixtureId", getFixtureHandler)
#ifndef CREATURE_UWS_OTEL_GATE
            .del("/api/v1/fixture/:fixtureId",
                 [fixtures, &blockingExecutor, &dispatcher](auto *response, auto *request) {
                     const std::string fixtureId(request->getParameter("fixtureId"));
                     if (!isUuid(fixtureId)) {
                         sendStatus(response, 400, "fixtureId must be an RFC 4122 UUID");
                         return;
                     }
                     dispatchPreparedResponse(
                         response, blockingExecutor, dispatcher,
                         [fixtures, fixtureId](std::stop_token) -> std::optional<PreparedHttpResponse> {
                             return fixtures->erase(fixtureId) ? preparedStatus(200, "Fixture deleted")
                                                               : preparedStatus(404, "Fixture not found");
                         });
                 })
            .get("/__spike/blocking/:milliseconds",
                 [&blockingExecutor, &dispatcher](auto *response, auto *request) {
                     const auto delay = parseDelay(request->getParameter("milliseconds"));
                     if (!delay.has_value()) {
                         sendStatus(response, 400, "Delay must be between 0 and 10000 milliseconds");
                         return;
                     }
                     dispatchBlockingResponse(response, blockingExecutor, dispatcher, delay.value());
                 })
#endif
            .post("/__spike/broadcast",
                  [&app, &blockingExecutor, &dispatcher](auto *response, auto *request) {
#ifndef CREATURE_UWS_OTEL_GATE
                      static_cast<void>(request);
#endif
#ifdef CREATURE_UWS_OTEL_GATE
                      auto requestSpan = creatures::observability->createRequestSpan(
                          "POST /__spike/broadcast", "POST", "/__spike/broadcast",
                          std::string(request->getHeader("traceparent")));
                      auto broadcastSpan =
                          creatures::observability->createLinkedOperationSpan("WebSocket.broadcast", requestSpan);
                      auto broadcastTrace = std::make_shared<AsyncTraceCompletion>(broadcastSpan);
                      if (requestSpan) {
                          requestSpan->setAttribute("http.route", "/__spike/broadcast");
                          requestSpan->setAttribute("transport.framework", "uwebsockets");
                      }
                      if (broadcastSpan) {
                          broadcastSpan->setAttribute("messaging.destination.name", BROADCAST_TOPIC.data());
                          broadcastSpan->setAttribute("messaging.operation", "publish");
                      }
#endif
                      const bool accepted = blockingExecutor.trySubmit([&app, &dispatcher
#ifdef CREATURE_UWS_OTEL_GATE
                                                                        ,
                                                                        broadcastTrace
#endif
                      ](std::stop_token stopToken) {
                          if (!interruptibleDelay(stopToken, 20)) {
#ifdef CREATURE_UWS_OTEL_GATE
                              broadcastTrace->fail("shutdown_cancelled", "Broadcast cancelled during shutdown");
#endif
                              return;
                          }
                          const bool deferred = dispatcher.post([&app
#ifdef CREATURE_UWS_OTEL_GATE
                                                                 ,
                                                                 broadcastTrace
#endif
                          ] {
                              const auto message =
                                  nlohmann::json{{"command", "concurrency_gate"}, {"payload", {{"source", "worker"}}}}
                                      .dump();
                              const bool published = app.publish(BROADCAST_TOPIC, message, uWS::OpCode::TEXT);
#ifdef CREATURE_UWS_OTEL_GATE
                              if (broadcastTrace->span()) {
                                  broadcastTrace->span()->setAttribute("messaging.message.body.size",
                                                                       static_cast<int64_t>(message.size()));
                                  broadcastTrace->span()->setAttribute("messaging.publish.accepted", published);
                              }
                              broadcastTrace->success(published ? "published" : "no_subscribers");
#else
                              static_cast<void>(published);
#endif
                          });
#ifdef CREATURE_UWS_OTEL_GATE
                          if (!deferred) {
                              broadcastTrace->fail("defer_failed", "Unable to return broadcast to transport loop");
                          }
#else
                          static_cast<void>(deferred);
#endif
                      });
                      if (!accepted) {
#ifdef CREATURE_UWS_OTEL_GATE
                          broadcastTrace->fail("queue_saturated", "Blocking work queue is full");
                          if (requestSpan) {
                              requestSpan->setAttribute("transport.outcome", "queue_saturated");
                              requestSpan->setError("Blocking work queue is full");
                              requestSpan->setHttpStatus(503);
                          }
#endif
                          sendStatus(response, 503, "Blocking work queue is full");
                          return;
                      }
#ifdef CREATURE_UWS_OTEL_GATE
                      if (requestSpan) {
                          requestSpan->setAttribute("transport.outcome", "accepted");
                          requestSpan->setHttpStatus(202);
                      }
#endif
                      sendJson(response, "202 Accepted", nlohmann::json{{"status", "accepted"}}.dump());
                  })
#ifndef CREATURE_UWS_OTEL_GATE
            .post("/__spike/broadcast/burst",
                  [&app, &blockingExecutor, &dispatcher](auto *response, auto *) {
                      const bool accepted = blockingExecutor.trySubmit([&app, &dispatcher](std::stop_token stopToken) {
                          auto message = std::make_shared<std::string>(256 * 1024, 'x');
                          if (stopToken.stop_requested()) {
                              return;
                          }
                          static_cast<void>(dispatcher.post([&app, message = std::move(message)] {
                              for (unsigned int index = 0; index < 32; ++index) {
                                  static_cast<void>(app.publish(BROADCAST_TOPIC, *message, uWS::OpCode::TEXT));
                              }
                          }));
                      });
                      if (!accepted) {
                          sendStatus(response, 503, "Blocking work queue is full");
                          return;
                      }
                      sendJson(response, "202 Accepted", nlohmann::json{{"status", "accepted"}}.dump());
                  })
#endif
            .ws<WebSocketState>(
                "/api/v1/websocket",
                {.compression = uWS::DISABLED,
                 .maxPayloadLength = WEBSOCKET_MESSAGE_LIMIT,
                 .idleTimeout = 120,
                 .maxBackpressure = 64 * 1024,
                 .closeOnBackpressureLimit = true,
                 .resetIdleTimeoutOnSend = false,
                 .sendPingsAutomatically = true,
                 .maxLifetime = 0,
#ifdef CREATURE_UWS_OTEL_GATE
                 .upgrade =
                     [](auto *response, auto *request, auto *context) {
                         auto upgradeSpan = creatures::observability->createRequestSpan(
                             "GET /api/v1/websocket", "GET", "/api/v1/websocket",
                             std::string(request->getHeader("traceparent")));
                         if (upgradeSpan) {
                             upgradeSpan->setAttribute("http.route", "/api/v1/websocket");
                             upgradeSpan->setAttribute("transport.framework", "uwebsockets");
                             upgradeSpan->setAttribute("transport.outcome", "upgraded");
                             upgradeSpan->setHttpStatus(101);
                         }
                         response->template upgrade<WebSocketState>(
                             WebSocketState{.upgradeSpan = std::move(upgradeSpan)},
                             request->getHeader("sec-websocket-key"), request->getHeader("sec-websocket-protocol"),
                             request->getHeader("sec-websocket-extensions"), context);
                     },
#endif
                 .open =
                     [](auto *socket) {
                         static_cast<void>(socket->subscribe(BROADCAST_TOPIC));
#ifdef CREATURE_UWS_OTEL_GATE
                         auto *state = socket->getUserData();
                         state->upgradeSpan.reset();
                         state->sessionSpan = creatures::observability->createSamplingSpan("WebSocket.session", 0.0005);
                         if (state->sessionSpan) {
                             state->sessionSpan->setAttribute("http.route", "/api/v1/websocket");
                             state->sessionSpan->setAttribute("transport.framework", "uwebsockets");
                             state->sessionSpan->setAttribute("websocket.sample_rate", 0.0005);
                         }
#endif
                     },
                 .message =
                     [](auto *socket, std::string_view message, uWS::OpCode opcode) {
#ifdef CREATURE_UWS_OTEL_GATE
                         auto *state = socket->getUserData();
                         if (state->sessionSpan) {
                             state->sessionSpan->setAttribute("websocket.message.size",
                                                              static_cast<int64_t>(message.size()));
                         }
#endif
                         if (opcode != uWS::OpCode::TEXT) {
                             return;
                         }
                         try {
                             static_cast<void>(nlohmann::json::parse(message));
                         } catch (const nlohmann::json::exception &error) {
#ifdef CREATURE_UWS_OTEL_GATE
                             state->failed = true;
                             if (state->sessionSpan) {
                                 state->sessionSpan->setAttribute("error.type", "MalformedWebSocketJson");
                                 state->sessionSpan->recordException(error);
                                 state->sessionSpan->setError("Malformed WebSocket JSON");
                             }
#endif
                             socket->send(
                                 nlohmann::json{{"command", "notice"},
                                                {"payload", {{"message", "Dropped malformed WebSocket message."}}}}
                                     .dump(),
                                 uWS::OpCode::TEXT);
                         }
                     },
                 .close =
                     [](auto *socket, int code, std::string_view reason) {
#ifdef CREATURE_UWS_OTEL_GATE
                         auto *state = socket->getUserData();
                         if (state->sessionSpan) {
                             state->sessionSpan->setAttribute("websocket.close.code", static_cast<int64_t>(code));
                             state->sessionSpan->setAttribute("websocket.close.reason", std::string(reason));
                             state->sessionSpan->setAttribute("transport.outcome",
                                                              state->failed ? "message_error" : "closed");
                             if (!state->failed) {
                                 state->sessionSpan->setSuccess();
                             }
                         }
#else
                          static_cast<void>(socket);
                          static_cast<void>(code);
                          static_cast<void>(reason);
#endif
                     }})
            .any("/*", [](auto *response, auto *) { sendStatus(response, 404, "Route not found"); });

        bool listening = false;
        app.listen("127.0.0.1", port, [port, &soundsRoot, &listening](auto *socket) {
            if (socket == nullptr) {
                return;
            }
            listening = true;
            std::cout << "µWebSockets transport spike listening on 127.0.0.1:" << port << " with sounds at "
                      << soundsRoot << std::endl;
        });
        if (!listening) {
            throw std::runtime_error("µWebSockets failed to listen on port " + std::to_string(port));
        }

        std::jthread shutdownThread([&app, &blockingExecutor, &fileExecutor, &dispatcher, &shutdownSignals] {
            int receivedSignal = 0;
            if (sigwait(&shutdownSignals, &receivedSignal) != 0) {
                return;
            }
            blockingExecutor.requestStop();
            fileExecutor.requestStop();
            dispatcher.closeAndPost([&app] { app.close(); });
            blockingExecutor.join();
            fileExecutor.join();
        });

        app.run();
        shutdownThread.join();
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "µWebSockets transport spike failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
