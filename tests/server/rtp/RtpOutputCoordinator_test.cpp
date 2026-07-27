#include <chrono>
#include <future>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/rtp/RtpOutputCoordinator.h"

namespace creatures::rtp {

TEST(RtpOutputCoordinator, NewOwnerInvalidatesPreviousGeneration) {
    RtpOutputCoordinator coordinator;
    const auto first = coordinator.acquire("first");
    const auto second = coordinator.acquire("second");

    EXPECT_LT(first.generation, second.generation);
    EXPECT_FALSE(coordinator.isCurrent(first));
    EXPECT_TRUE(coordinator.isCurrent(second));
    EXPECT_EQ(coordinator.activeOwnerId(), "second");
}

TEST(RtpOutputCoordinator, StaleReleaseCannotClearCurrentOwner) {
    RtpOutputCoordinator coordinator;
    const auto first = coordinator.acquire("first");
    const auto second = coordinator.acquire("second");

    coordinator.release(first);

    EXPECT_TRUE(coordinator.isCurrent(second));
    EXPECT_EQ(coordinator.activeGeneration(), second.generation);
}

TEST(RtpOutputCoordinator, CurrentReleaseInvalidatesLease) {
    RtpOutputCoordinator coordinator;
    const auto lease = coordinator.acquire("owner");

    coordinator.release(lease);

    EXPECT_FALSE(coordinator.isCurrent(lease));
    EXPECT_EQ(coordinator.activeGeneration(), 0U);
    EXPECT_TRUE(coordinator.activeOwnerId().empty());
}

TEST(RtpOutputCoordinator, WorkerGuardRejectsStaleCommands) {
    RtpOutputCoordinator coordinator;
    const auto stale = coordinator.acquire("stale");
    const auto current = coordinator.acquire("current");

    EXPECT_FALSE(static_cast<bool>(coordinator.lockIfCurrent(stale)));
    EXPECT_TRUE(static_cast<bool>(coordinator.lockIfCurrent(current)));
}

TEST(RtpOutputCoordinator, ReplacementRejectsOldResetPrimingAndAudioWork) {
    RtpOutputCoordinator coordinator;
    std::vector<std::string> emitted;
    const auto emit = [&coordinator, &emitted](const RtpOutputLease &lease, std::string kind) {
        auto guard = coordinator.lockIfCurrent(lease);
        if (!guard) {
            return false;
        }
        emitted.push_back(std::move(kind));
        return true;
    };

    const auto beforeReset = coordinator.acquire("before-reset");
    const auto resetReplacement = coordinator.acquire("reset-replacement");
    EXPECT_FALSE(emit(beforeReset, "old-reset"));
    EXPECT_TRUE(emit(resetReplacement, "new-reset"));

    const auto duringPriming = coordinator.acquire("during-priming");
    EXPECT_TRUE(emit(duringPriming, "old-silence-before-replacement"));
    const auto primingReplacement = coordinator.acquire("priming-replacement");
    EXPECT_FALSE(emit(duringPriming, "old-silence-after-replacement"));
    EXPECT_TRUE(emit(primingReplacement, "new-silence"));

    const auto duringAudio = coordinator.acquire("during-audio");
    EXPECT_TRUE(emit(duringAudio, "old-audio-before-replacement"));
    const auto audioReplacement = coordinator.acquire("audio-replacement");
    EXPECT_FALSE(emit(duringAudio, "old-audio-after-replacement"));
    EXPECT_TRUE(emit(audioReplacement, "new-audio"));

    EXPECT_EQ(emitted, (std::vector<std::string>{"new-reset", "old-silence-before-replacement", "new-silence",
                                                 "old-audio-before-replacement", "new-audio"}));
}

TEST(RtpOutputCoordinator, AcquireReturnsOnlyAfterInflightChannelSetFinishes) {
    using namespace std::chrono_literals;

    RtpOutputCoordinator coordinator;
    const auto first = coordinator.acquire("first");
    std::promise<void> guardEntered;
    std::promise<void> releaseGuard;
    auto releaseFuture = releaseGuard.get_future();

    auto sender = std::async(std::launch::async, [&] {
        auto guard = coordinator.lockIfCurrent(first);
        guardEntered.set_value();
        releaseFuture.wait();
        return static_cast<bool>(guard);
    });
    guardEntered.get_future().wait();

    std::promise<void> acquireStarted;
    auto replacement = std::async(std::launch::async, [&] {
        acquireStarted.set_value();
        return coordinator.acquire("replacement");
    });
    acquireStarted.get_future().wait();
    EXPECT_EQ(replacement.wait_for(20ms), std::future_status::timeout);

    releaseGuard.set_value();
    EXPECT_TRUE(sender.get());
    const auto second = replacement.get();

    EXPECT_TRUE(coordinator.isCurrent(second));
    EXPECT_FALSE(static_cast<bool>(coordinator.lockIfCurrent(first)));
}

} // namespace creatures::rtp
