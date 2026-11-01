#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "server/transport/ApplicationExecutor.h"

namespace creatures::transport {
namespace {

TEST(ApplicationExecutor, RejectsWorkWhenItsQueueIsFull) {
    ApplicationExecutor executor(1, 1);
    std::mutex mutex;
    std::condition_variable condition;
    bool running = false;
    bool release = false;

    ASSERT_TRUE(executor.trySubmit([&](std::stop_token) {
        std::unique_lock lock(mutex);
        running = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
    }));
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return running; });
    }

    ASSERT_TRUE(executor.trySubmit([](std::stop_token) {}));
    EXPECT_FALSE(executor.trySubmit([](std::stop_token) {}));

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    executor.requestStop();
    executor.join();
}

TEST(ApplicationExecutor, ReportsQueuedTasksAbandonedDuringShutdown) {
    ApplicationExecutor executor(1, 1);
    std::mutex mutex;
    std::condition_variable condition;
    bool running = false;
    bool release = false;
    std::atomic_bool abandoned{false};

    ASSERT_TRUE(executor.trySubmit([&](std::stop_token) {
        std::unique_lock lock(mutex);
        running = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
    }));
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return running; });
    }
    ASSERT_TRUE(executor.trySubmit([](std::stop_token) {}, [&] { abandoned.store(true); }));

    executor.requestStop();
    EXPECT_TRUE(abandoned.load());
    EXPECT_TRUE(executor.stopping());
    EXPECT_EQ(executor.queued(), 0);

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    executor.join();
}

} // namespace
} // namespace creatures::transport
