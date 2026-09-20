#include <gtest/gtest.h>

#include "server/transport/RequestRegistry.h"

namespace creatures::transport {
namespace {

TEST(RequestRegistry, CompletionConsumesOnlyTheMatchingGeneration) {
    RequestRegistry registry;
    int response = 1;
    const auto token = registry.add(&response, nullptr, true);

    EXPECT_FALSE(registry.take({token.id, token.generation + 1}).has_value());
    ASSERT_EQ(registry.size(), 1);
    const auto completed = registry.take(token);
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->response, &response);
    EXPECT_TRUE(completed->headOnly);
    EXPECT_EQ(registry.size(), 0);
    EXPECT_FALSE(registry.take(token).has_value());
}

TEST(RequestRegistry, AbortInvalidatesLateCompletion) {
    RequestRegistry registry;
    int response = 1;
    const auto token = registry.add(&response, nullptr, false);

    EXPECT_TRUE(registry.abort(token, "client_aborted", "client left", 499));
    EXPECT_FALSE(registry.take(token).has_value());
    EXPECT_FALSE(registry.abort(token, "client_aborted", "client left", 499));
}

TEST(RequestRegistry, ShutdownCancelsEveryOutstandingRequest) {
    RequestRegistry registry;
    int first = 1;
    int second = 2;
    registry.add(&first, nullptr, false);
    registry.add(&second, nullptr, true);

    const auto cancelled = registry.cancelAll("shutdown_cancelled", "server shutting down", 503);
    EXPECT_EQ(cancelled.size(), 2);
    EXPECT_EQ(registry.size(), 0);
}

} // namespace
} // namespace creatures::transport
