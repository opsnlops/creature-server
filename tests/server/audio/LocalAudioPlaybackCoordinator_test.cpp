#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "server/audio/LocalAudioPlaybackCoordinator.h"

namespace creatures::audio {
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

TEST(LocalAudioPlaybackCoordinatorTest, FloodKeepsOneWorkerAndOnePendingJob) {
    LocalAudioPlaybackCoordinator coordinator;
    std::latch activeStarted(1);
    std::atomic<bool> releaseActive{false};
    std::atomic<int> jobsRun{0};
    std::atomic<int> runningJobs{0};
    std::atomic<int> maxRunningJobs{0};

    auto recordConcurrency = [&]() {
        const int current = runningJobs.fetch_add(1) + 1;
        int observed = maxRunningJobs.load();
        while (current > observed && !maxRunningJobs.compare_exchange_weak(observed, current)) {
        }
    };

    auto first = coordinator.submit(
        {.id = "first", .source = "test", .fileName = "first.wav", .play = [&](const std::atomic<bool> &) {
             jobsRun.fetch_add(1);
             recordConcurrency();
             activeStarted.count_down();
             while (!releaseActive.load()) {
                 std::this_thread::sleep_for(1ms);
             }
             runningJobs.fetch_sub(1);
             return LocalAudioPlaybackCoordinator::PlaybackResult{};
         }});
    ASSERT_EQ(first.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);
    activeStarted.wait();

    std::vector<std::shared_ptr<LocalAudioPlaybackCoordinator::Handle>> handles;
    handles.reserve(1000);
    for (int index = 0; index < 1000; ++index) {
        auto submission = coordinator.submit({.id = "flood-" + std::to_string(index),
                                              .source = "test",
                                              .fileName = "sound.wav",
                                              .play = [&](const std::atomic<bool> &) {
                                                  jobsRun.fetch_add(1);
                                                  recordConcurrency();
                                                  runningJobs.fetch_sub(1);
                                                  return LocalAudioPlaybackCoordinator::PlaybackResult{};
                                              }});
        ASSERT_EQ(submission.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);
        handles.push_back(std::move(submission.handle));
        const auto stats = coordinator.getStats();
        EXPECT_EQ(stats.workerCount, 1U);
        EXPECT_LE(stats.active, 1U);
        EXPECT_LE(stats.queued, 1U);
    }

    releaseActive.store(true);
    ASSERT_TRUE(waitUntil([&]() { return coordinator.getStats().active == 0 && coordinator.getStats().queued == 0; }));

    EXPECT_LE(jobsRun.load(), 2);
    EXPECT_EQ(maxRunningJobs.load(), 1);
    EXPECT_EQ(coordinator.getStats().accepted, 1001U);
    EXPECT_EQ(coordinator.getStats().replaced, 1000U);
}

TEST(LocalAudioPlaybackCoordinatorTest, LastRequestStopsActiveAndRunsNext) {
    LocalAudioPlaybackCoordinator coordinator;
    std::latch firstStarted(1);
    std::promise<void> secondRanPromise;
    auto secondRan = secondRanPromise.get_future();
    std::atomic<bool> firstSawStop{false};

    auto first = coordinator.submit({.id = "first",
                                     .source = "animation",
                                     .fileName = "first.wav",
                                     .play = [&](const std::atomic<bool> &stopRequested) {
                                         firstStarted.count_down();
                                         while (!stopRequested.load()) {
                                             std::this_thread::sleep_for(1ms);
                                         }
                                         firstSawStop.store(true);
                                         return LocalAudioPlaybackCoordinator::PlaybackResult{
                                             .outcome = LocalAudioPlaybackCoordinator::PlaybackOutcome::Stopped};
                                     }});
    ASSERT_EQ(first.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);
    firstStarted.wait();

    auto second = coordinator.submit(
        {.id = "second", .source = "standalone", .fileName = "second.wav", .play = [&](const std::atomic<bool> &) {
             secondRanPromise.set_value();
             return LocalAudioPlaybackCoordinator::PlaybackResult{};
         }});
    ASSERT_EQ(second.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);

    ASSERT_EQ(secondRan.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(firstSawStop.load());
    ASSERT_TRUE(waitUntil([&]() { return first.handle->isFinished() && second.handle->isFinished(); }));
    EXPECT_EQ(coordinator.getStats().replaced, 1U);
    EXPECT_EQ(coordinator.getStats().completed, 1U);
}

TEST(LocalAudioPlaybackCoordinatorTest, ExplicitStopIsCooperativeAndNonBlocking) {
    LocalAudioPlaybackCoordinator coordinator;
    std::latch started(1);
    std::atomic<bool> observedStop{false};

    auto submission = coordinator.submit({.id = "stop",
                                          .source = "animation",
                                          .fileName = "sound.wav",
                                          .play = [&](const std::atomic<bool> &stopRequested) {
                                              started.count_down();
                                              while (!stopRequested.load()) {
                                                  std::this_thread::sleep_for(1ms);
                                              }
                                              observedStop.store(true);
                                              return LocalAudioPlaybackCoordinator::PlaybackResult{};
                                          }});
    ASSERT_EQ(submission.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);
    started.wait();

    const auto before = std::chrono::steady_clock::now();
    submission.handle->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 50ms);
    ASSERT_TRUE(waitUntil([&]() { return submission.handle->isFinished(); }));
    EXPECT_TRUE(observedStop.load());
    EXPECT_EQ(coordinator.getStats().stopped, 1U);
}

TEST(LocalAudioPlaybackCoordinatorTest, RecordsFailureAndTimeoutOutcomes) {
    LocalAudioPlaybackCoordinator coordinator;

    auto failed = coordinator.submit(
        {.id = "failed", .source = "test", .fileName = "failed.wav", .play = [](const std::atomic<bool> &) {
             return LocalAudioPlaybackCoordinator::PlaybackResult{
                 .outcome = LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed,
                 .errorCode = "local_audio.open_failed",
                 .errorMessage = "no device"};
         }});
    ASSERT_EQ(failed.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);
    ASSERT_TRUE(waitUntil([&]() { return failed.handle->isFinished(); }));

    auto timedOut = coordinator.submit(
        {.id = "timeout", .source = "test", .fileName = "timeout.wav", .play = [](const std::atomic<bool> &) {
             return LocalAudioPlaybackCoordinator::PlaybackResult{
                 .outcome = LocalAudioPlaybackCoordinator::PlaybackOutcome::TimedOut,
                 .errorCode = "local_audio.playback_timeout",
                 .errorMessage = "deadline exceeded"};
         }});
    ASSERT_EQ(timedOut.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);
    ASSERT_TRUE(waitUntil([&]() { return timedOut.handle->isFinished(); }));

    EXPECT_EQ(coordinator.getStats().failed, 1U);
    EXPECT_EQ(coordinator.getStats().timedOut, 1U);
}

TEST(LocalAudioPlaybackCoordinatorTest, ContainsThrownPlaybackFailureAndRunsTheNextJob) {
    LocalAudioPlaybackCoordinator coordinator;
    std::promise<LocalAudioPlaybackCoordinator::Completion> failedCompletionPromise;
    auto failedCompletion = failedCompletionPromise.get_future();

    auto failed =
        coordinator.submit({.id = "throws",
                            .source = "test",
                            .fileName = "throws.wav",
                            .play = [](const std::atomic<bool> &) -> LocalAudioPlaybackCoordinator::PlaybackResult {
                                throw std::runtime_error("device exploded");
                            },
                            .onFinished =
                                [&](const LocalAudioPlaybackCoordinator::Completion &completion) {
                                    failedCompletionPromise.set_value(completion);
                                }});
    ASSERT_EQ(failed.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);
    ASSERT_EQ(failedCompletion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(failedCompletion.get().result.outcome, LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed);

    auto next = coordinator.submit(
        {.id = "next", .source = "test", .fileName = "next.wav", .play = [](const std::atomic<bool> &) {
             return LocalAudioPlaybackCoordinator::PlaybackResult{};
         }});
    ASSERT_EQ(next.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);
    ASSERT_TRUE(waitUntil([&]() { return next.handle->isFinished(); }));
    EXPECT_EQ(coordinator.getStats().failed, 1U);
    EXPECT_EQ(coordinator.getStats().completed, 1U);
}

TEST(LocalAudioPlaybackCoordinatorTest, ReplacedPendingJobFinishesWithoutRunning) {
    LocalAudioPlaybackCoordinator coordinator;
    std::latch activeStarted(1);
    std::atomic<bool> releaseActive{false};
    std::atomic<bool> displacedRan{false};
    std::promise<LocalAudioPlaybackCoordinator::Completion> displacedCompletionPromise;
    auto displacedCompletion = displacedCompletionPromise.get_future();

    auto active = coordinator.submit(
        {.id = "active", .source = "test", .fileName = "active.wav", .play = [&](const std::atomic<bool> &) {
             activeStarted.count_down();
             while (!releaseActive.load()) {
                 std::this_thread::sleep_for(1ms);
             }
             return LocalAudioPlaybackCoordinator::PlaybackResult{};
         }});
    ASSERT_EQ(active.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);
    activeStarted.wait();

    auto displaced = coordinator.submit({.id = "displaced",
                                         .source = "test",
                                         .fileName = "displaced.wav",
                                         .play =
                                             [&](const std::atomic<bool> &) {
                                                 displacedRan.store(true);
                                                 return LocalAudioPlaybackCoordinator::PlaybackResult{};
                                             },
                                         .onFinished =
                                             [&](const LocalAudioPlaybackCoordinator::Completion &completion) {
                                                 displacedCompletionPromise.set_value(completion);
                                             }});
    ASSERT_EQ(displaced.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);

    auto latest = coordinator.submit(
        {.id = "latest", .source = "test", .fileName = "latest.wav", .play = [](const std::atomic<bool> &) {
             return LocalAudioPlaybackCoordinator::PlaybackResult{};
         }});
    ASSERT_EQ(latest.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);
    ASSERT_EQ(displacedCompletion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(displacedCompletion.get().result.outcome, LocalAudioPlaybackCoordinator::PlaybackOutcome::Replaced);
    EXPECT_TRUE(displaced.handle->isFinished());
    EXPECT_FALSE(displacedRan.load());

    releaseActive.store(true);
}

TEST(LocalAudioPlaybackCoordinatorTest, ShutdownCancelsActiveAndPendingAndRejectsNewWork) {
    auto coordinator = std::make_unique<LocalAudioPlaybackCoordinator>();
    std::latch activeStarted(1);

    auto active = coordinator->submit({.id = "active",
                                       .source = "test",
                                       .fileName = "active.wav",
                                       .play = [&](const std::atomic<bool> &stopRequested) {
                                           activeStarted.count_down();
                                           while (!stopRequested.load()) {
                                               std::this_thread::sleep_for(1ms);
                                           }
                                           return LocalAudioPlaybackCoordinator::PlaybackResult{};
                                       }});
    ASSERT_EQ(active.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);
    activeStarted.wait();

    auto pending = coordinator->submit(
        {.id = "pending", .source = "test", .fileName = "pending.wav", .play = [](const std::atomic<bool> &) {
             return LocalAudioPlaybackCoordinator::PlaybackResult{};
         }});
    ASSERT_EQ(pending.result, LocalAudioPlaybackCoordinator::SubmitResult::Accepted);

    auto shutdown = std::async(std::launch::async, [&]() { coordinator->shutdown(); });
    ASSERT_EQ(shutdown.wait_for(2s), std::future_status::ready);
    shutdown.get();
    EXPECT_TRUE(active.handle->isFinished());
    EXPECT_TRUE(pending.handle->isFinished());

    auto late = coordinator->submit(
        {.id = "late", .source = "test", .fileName = "late.wav", .play = [](const std::atomic<bool> &) {
             return LocalAudioPlaybackCoordinator::PlaybackResult{};
         }});
    EXPECT_EQ(late.result, LocalAudioPlaybackCoordinator::SubmitResult::ShuttingDown);
    EXPECT_EQ(coordinator->getStats().rejected, 1U);
}

} // namespace
} // namespace creatures::audio
