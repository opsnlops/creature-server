#include "server/transport/RequestRegistry.h"

#include <cassert>
#include <utility>

namespace creatures::transport {

RequestRegistry::RequestRegistry() : owner_(std::this_thread::get_id()) {}

RequestToken RequestRegistry::add(void *response, std::shared_ptr<RequestSpan> span, const bool headOnly) {
    assertOwner();
    const auto id = nextId_++;
    constexpr uint64_t generation = 1;
    requests_.emplace(id, State{generation, {response, std::move(span), headOnly}});
    return {id, generation};
}

std::shared_ptr<RequestSpan> RequestRegistry::span(const RequestToken token) const {
    assertOwner();
    const auto found = requests_.find(token.id);
    if (found == requests_.end() || found->second.generation != token.generation) {
        return nullptr;
    }
    return found->second.request.span;
}

std::optional<RegisteredRequest> RequestRegistry::take(const RequestToken token) {
    assertOwner();
    const auto found = requests_.find(token.id);
    if (found == requests_.end() || found->second.generation != token.generation) {
        return std::nullopt;
    }
    auto request = std::move(found->second.request);
    requests_.erase(found);
    return request;
}

bool RequestRegistry::abort(const RequestToken token, const std::string &outcome, const std::string &message,
                            const int statusCode) {
    assertOwner();
    auto request = take(token);
    if (!request.has_value()) {
        return false;
    }
    recordTerminal(request->span, outcome, message, statusCode);
    return true;
}

std::vector<RegisteredRequest> RequestRegistry::cancelAll(const std::string &outcome, const std::string &message,
                                                          const int statusCode) {
    assertOwner();
    std::vector<RegisteredRequest> cancelled;
    cancelled.reserve(requests_.size());
    for (auto &[id, state] : requests_) {
        (void)id;
        recordTerminal(state.request.span, outcome, message, statusCode);
        cancelled.push_back(std::move(state.request));
    }
    requests_.clear();
    return cancelled;
}

std::size_t RequestRegistry::size() const {
    assertOwner();
    return requests_.size();
}

void RequestRegistry::assertOwner() const { assert(owner_ == std::this_thread::get_id()); }

void RequestRegistry::recordTerminal(const std::shared_ptr<RequestSpan> &span, const std::string &outcome,
                                     const std::string &message, const int statusCode) {
    if (!span) {
        return;
    }
    span->setAttribute("transport.outcome", outcome);
    span->setAttribute("error.type", outcome);
    span->setAttribute("error.code", static_cast<int64_t>(statusCode));
    span->setAttribute("error.message", message);
    span->setError(message);
    span->setHttpStatus(statusCode);
}

} // namespace creatures::transport
