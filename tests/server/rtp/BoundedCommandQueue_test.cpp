#include <gtest/gtest.h>

#include "server/rtp/BoundedCommandQueue.h"

namespace creatures::rtp {

TEST(BoundedCommandQueue, RejectsWorkAtCapacityWithoutBlocking) {
    BoundedCommandQueue<int> queue(2);

    EXPECT_TRUE(queue.tryPush(10));
    EXPECT_TRUE(queue.tryPush(20));
    EXPECT_FALSE(queue.tryPush(30));
    EXPECT_EQ(queue.size(), 2U);

    const auto first = queue.waitPop();
    const auto second = queue.waitPop();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, 10);
    EXPECT_EQ(*second, 20);
}

TEST(BoundedCommandQueue, ErasesMatchingQueuedWork) {
    BoundedCommandQueue<int> queue(4);
    ASSERT_TRUE(queue.tryPush(1));
    ASSERT_TRUE(queue.tryPush(2));
    ASSERT_TRUE(queue.tryPush(3));

    EXPECT_EQ(queue.eraseIf([](int value) { return value % 2 == 1; }), 2U);
    EXPECT_EQ(queue.size(), 1U);
    EXPECT_EQ(*queue.waitPop(), 2);
}

TEST(BoundedCommandQueue, StopDiscardsPendingWorkAndRejectsProducers) {
    BoundedCommandQueue<int> queue(2);
    ASSERT_TRUE(queue.tryPush(1));

    queue.stop();

    EXPECT_FALSE(queue.waitPop().has_value());
    EXPECT_FALSE(queue.tryPush(2));
    EXPECT_EQ(queue.size(), 0U);
}

TEST(BoundedCommandQueue, MaximumDurationWorkCannotGrowPastCapacity) {
    constexpr size_t capacity = 64;
    constexpr size_t maximumCacheFrames = 2'000'000;
    BoundedCommandQueue<size_t> queue(capacity);

    size_t accepted = 0;
    for (size_t frame = 0; frame < maximumCacheFrames; ++frame) {
        if (queue.tryPush(frame)) {
            ++accepted;
        }
    }

    EXPECT_EQ(accepted, capacity);
    EXPECT_EQ(queue.size(), capacity);
}

} // namespace creatures::rtp
