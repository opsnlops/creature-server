#include <gtest/gtest.h>
#include <vector>

#include "support/FakeEventLoop.h"

namespace creatures::testing {

TEST(FakeEventLoopTest, ExecutesCallbacksInFrameOrder) {
    FakeEventLoop loop;
    std::vector<int> order;

    loop.schedule(5, [&] { order.push_back(2); });
    loop.schedule(1, [&] { order.push_back(0); });
    loop.schedule(3, [&] { order.push_back(1); });

    loop.runUntilEmpty();

    ASSERT_TRUE(loop.empty());
    EXPECT_EQ(loop.executedFrames(), std::vector<framenum_t>({1, 3, 5}));
    EXPECT_EQ(order, std::vector<int>({0, 1, 2}));
}

TEST(FakeEventLoopTest, HandlesNoopCallbacks) {
    FakeEventLoop loop;
    loop.schedule(2, nullptr);
    loop.schedule(1, [] {});

    loop.runUntilEmpty();

    EXPECT_EQ(loop.executedFrames(), std::vector<framenum_t>({1, 2}));
}

} // namespace creatures::testing
