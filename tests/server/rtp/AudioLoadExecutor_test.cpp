#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "server/rtp/AudioLoadExecutor.h"

namespace creatures::rtp {
namespace {

using namespace std::chrono_literals;

template <typename Predicate> bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

TEST(AudioLoadExecutorTest, RejectsImmediatelyWhenBoundedQueueIsFull) {
    AudioLoadExecutor executor(2, 3);
    std::atomic<std::size_t> workersStarted{0};
    std::atomic<bool> release{false};

    auto blockingJob = [&]() {
        workersStarted.fetch_add(1);
        while (!release.load()) {
            std::this_thread::sleep_for(1ms);
        }
    };
    ASSERT_EQ(executor.submit({.id = "active-1", .run = blockingJob}), AudioLoadExecutor::SubmitResult::Accepted);
    ASSERT_EQ(executor.submit({.id = "active-2", .run = blockingJob}), AudioLoadExecutor::SubmitResult::Accepted);
    ASSERT_TRUE(waitUntil([&]() { return workersStarted.load() == 2; }));

    ASSERT_EQ(executor.submit({.id = "queued-1", .run = [] {}}), AudioLoadExecutor::SubmitResult::Accepted);
    ASSERT_EQ(executor.submit({.id = "queued-2", .run = [] {}}), AudioLoadExecutor::SubmitResult::Accepted);
    ASSERT_EQ(executor.submit({.id = "queued-3", .run = [] {}}), AudioLoadExecutor::SubmitResult::Accepted);

    const auto before = std::chrono::steady_clock::now();
    EXPECT_EQ(executor.submit({.id = "rejected", .run = [] {}}), AudioLoadExecutor::SubmitResult::QueueFull);
    EXPECT_LT(std::chrono::steady_clock::now() - before, 50ms);

    auto stats = executor.getStats();
    EXPECT_EQ(stats.workerCount, 2U);
    EXPECT_EQ(stats.queueCapacity, 3U);
    EXPECT_EQ(stats.active, 2U);
    EXPECT_EQ(stats.queued, 3U);
    EXPECT_EQ(stats.rejected, 1U);

    release.store(true);
}

TEST(AudioLoadExecutorTest, SkipsCancelledQueuedJobBeforeRunningIt) {
    AudioLoadExecutor executor(1, 2);
    std::latch workerStarted(1);
    std::promise<void> releasePromise;
    auto release = releasePromise.get_future().share();
    std::atomic<bool> cancelled{false};
    std::atomic<bool> cancelledJobRan{false};

    ASSERT_EQ(executor.submit({.id = "active",
                               .run =
                                   [&]() {
                                       workerStarted.count_down();
                                       release.wait();
                                   }}),
              AudioLoadExecutor::SubmitResult::Accepted);
    workerStarted.wait();

    ASSERT_EQ(executor.submit({.id = "cancel-me",
                               .isCancelled = [&]() { return cancelled.load(); },
                               .run = [&]() { cancelledJobRan.store(true); }}),
              AudioLoadExecutor::SubmitResult::Accepted);
    cancelled.store(true);
    releasePromise.set_value();

    ASSERT_TRUE(waitUntil([&]() { return executor.getStats().cancelled == 1; }));
    EXPECT_FALSE(cancelledJobRan.load());
    EXPECT_EQ(executor.getStats().completed, 1U);
}

TEST(AudioLoadExecutorTest, NewSubmissionPrunesCancelledWorkBeforeCapacityCheck) {
    AudioLoadExecutor executor(1, 1);
    std::latch workerStarted(1);
    std::promise<void> releasePromise;
    auto release = releasePromise.get_future().share();
    std::atomic<bool> cancelled{false};
    std::atomic<bool> cancelledJobRan{false};
    std::atomic<bool> replacementRan{false};

    ASSERT_EQ(executor.submit({.id = "active",
                               .run =
                                   [&]() {
                                       workerStarted.count_down();
                                       release.wait();
                                   }}),
              AudioLoadExecutor::SubmitResult::Accepted);
    workerStarted.wait();
    ASSERT_EQ(executor.submit({.id = "cancel-me",
                               .isCancelled = [&]() { return cancelled.load(); },
                               .run = [&]() { cancelledJobRan.store(true); }}),
              AudioLoadExecutor::SubmitResult::Accepted);

    cancelled.store(true);
    EXPECT_EQ(executor.submit({.id = "replacement", .run = [&]() { replacementRan.store(true); }}),
              AudioLoadExecutor::SubmitResult::Accepted);
    EXPECT_EQ(executor.getStats().cancelled, 1U);
    EXPECT_EQ(executor.getStats().queued, 1U);

    releasePromise.set_value();
    ASSERT_TRUE(waitUntil([&]() { return replacementRan.load(); }));
    EXPECT_FALSE(cancelledJobRan.load());
}

TEST(AudioLoadExecutorTest, ContainsExceptionsAndInvokesFailureHandler) {
    AudioLoadExecutor executor(1, 1);
    std::promise<std::string> failurePromise;
    auto failure = failurePromise.get_future();

    ASSERT_EQ(executor.submit({.id = "throws",
                               .run = []() { throw std::runtime_error("encode exploded"); },
                               .onFailure =
                                   [&](std::exception_ptr exception) {
                                       try {
                                           std::rethrow_exception(exception);
                                       } catch (const std::exception &ex) {
                                           failurePromise.set_value(ex.what());
                                       }
                                   }}),
              AudioLoadExecutor::SubmitResult::Accepted);

    ASSERT_EQ(failure.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(failure.get(), "encode exploded");
    ASSERT_TRUE(waitUntil([&]() { return executor.getStats().active == 0; }));
    EXPECT_EQ(executor.getStats().failed, 1U);
    EXPECT_EQ(executor.getStats().completed, 0U);
}

TEST(AudioLoadExecutorTest, ShutdownDiscardsPendingJobsAndRejectsNewOnes) {
    auto executor = std::make_unique<AudioLoadExecutor>(1, 2);
    std::latch workerStarted(1);
    std::promise<void> releasePromise;
    auto release = releasePromise.get_future().share();
    std::atomic<bool> pendingRan{false};

    ASSERT_EQ(executor->submit({.id = "active",
                                .run =
                                    [&]() {
                                        workerStarted.count_down();
                                        release.wait();
                                    }}),
              AudioLoadExecutor::SubmitResult::Accepted);
    workerStarted.wait();
    ASSERT_EQ(executor->submit({.id = "pending", .run = [&]() { pendingRan.store(true); }}),
              AudioLoadExecutor::SubmitResult::Accepted);

    auto shutdown = std::async(std::launch::async, [&]() { executor->shutdown(); });
    ASSERT_TRUE(waitUntil([&]() { return executor->getStats().queued == 0; }));
    releasePromise.set_value();
    ASSERT_EQ(shutdown.wait_for(2s), std::future_status::ready);
    shutdown.get();

    EXPECT_FALSE(pendingRan.load());
    EXPECT_EQ(executor->getStats().cancelled, 1U);
    EXPECT_EQ(executor->submit({.id = "late", .run = [] {}}), AudioLoadExecutor::SubmitResult::ShuttingDown);
    EXPECT_EQ(executor->getStats().rejected, 1U);
}

TEST(AudioLoadExecutorTest, ObserverFinishesWithLatestConcurrentGaugeValues) {
    std::mutex observedMutex;
    AudioLoadExecutor::Stats observed;
    AudioLoadExecutor executor(4, 16, [&](const AudioLoadExecutor::Stats &stats) {
        std::lock_guard lock(observedMutex);
        observed = stats;
    });

    for (std::size_t index = 0; index < 16; ++index) {
        ASSERT_EQ(
            executor.submit({.id = "job-" + std::to_string(index), .run = []() { std::this_thread::sleep_for(1ms); }}),
            AudioLoadExecutor::SubmitResult::Accepted);
    }
    ASSERT_TRUE(waitUntil([&]() {
        const auto stats = executor.getStats();
        return stats.completed == 16 && stats.active == 0 && stats.queued == 0;
    }));

    std::lock_guard lock(observedMutex);
    EXPECT_EQ(observed.completed, 16U);
    EXPECT_EQ(observed.active, 0U);
    EXPECT_EQ(observed.queued, 0U);
}

} // namespace
} // namespace creatures::rtp
