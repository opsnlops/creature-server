#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "server/transport/HttpTypes.h"
#include "util/ObservabilityManager.h"

namespace creatures::transport {

struct RequestToken {
    uint64_t id{0};
    uint64_t generation{0};

    bool operator==(const RequestToken &) const = default;
};

struct RegisteredRequest {
    void *response{nullptr};
    std::shared_ptr<RequestSpan> span;
    bool headOnly{false};
};

/**
 * Loop-thread-owned registry for every live HTTP response and request span.
 * Workers carry only RequestToken values and immutable completion data.
 */
class RequestRegistry {
  public:
    RequestRegistry();

    RequestToken add(void *response, std::shared_ptr<RequestSpan> span, bool headOnly);
    [[nodiscard]] std::shared_ptr<RequestSpan> span(RequestToken token) const;
    /** Copy of a live registration without removing it; streamed responses stay registered until they finish. */
    [[nodiscard]] std::optional<RegisteredRequest> peek(RequestToken token) const;
    std::optional<RegisteredRequest> take(RequestToken token);
    bool abort(RequestToken token, const std::string &outcome, const std::string &message, int statusCode);
    std::vector<RegisteredRequest> cancelAll(const std::string &outcome, const std::string &message, int statusCode);

    [[nodiscard]] std::size_t size() const;

    /**
     * The loop's file streamer, when one is running. A registered response
     * whose PreparedResponse carries a file is handed here and stays
     * registered until the stream finishes; without a starter such a
     * response cannot be served.
     */
    using FileStreamStarter = std::function<void(RequestToken, const PreparedResponse &)>;
    void setFileStreamStarter(FileStreamStarter starter);
    [[nodiscard]] bool hasFileStreamStarter() const { return static_cast<bool>(fileStreamStarter_); }
    void startFileStream(RequestToken token, const PreparedResponse &prepared) const;

  private:
    struct State {
        uint64_t generation;
        RegisteredRequest request;
    };

    void assertOwner() const;
    static void recordTerminal(const std::shared_ptr<RequestSpan> &span, const std::string &outcome,
                               const std::string &message, int statusCode);

    std::thread::id owner_;
    uint64_t nextId_{1};
    std::unordered_map<uint64_t, State> requests_;
    FileStreamStarter fileStreamStarter_;
};

} // namespace creatures::transport
