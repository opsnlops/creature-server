#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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
    std::optional<RegisteredRequest> take(RequestToken token);
    bool abort(RequestToken token, const std::string &outcome, const std::string &message, int statusCode);
    std::vector<RegisteredRequest> cancelAll(const std::string &outcome, const std::string &message, int statusCode);

    [[nodiscard]] std::size_t size() const;

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
};

} // namespace creatures::transport
