// Deterministic tests for the RTP send-failure circuit breaker (issue #97).
//
// MultiOpusRtpServer itself opens 17 real multicast sockets in its
// constructor, so the breaker logic lives in header-only classes that these
// tests drive directly with injected time.

#include <chrono>

#include <gtest/gtest.h>

#include "server/rtp/RtpOutputHealth.h"

namespace creatures::rtp {

namespace {

using std::chrono::seconds;
using TimePoint = std::chrono::steady_clock::time_point;

// A fixed epoch keeps every test independent of the wall clock.
TimePoint t0() { return TimePoint{seconds(1000)}; }

RtpBreakerConfig smallConfig() {
    RtpBreakerConfig config;
    config.consecutiveFailuresToTrip = 10;
    config.windowedFailuresToTrip = 20;
    config.windowSeconds = 5;
    config.detailEmitInterval = 4;
    return config;
}

} // namespace

TEST(RtpSendFailureTracker, FirstFailureEmitsDetailThenGates) {
    RtpSendFailureTracker tracker(smallConfig());
    tracker.beginGeneration(1, t0());

    auto first = tracker.recordFailure(1, t0());
    EXPECT_TRUE(first.emitDetail);
    EXPECT_FALSE(first.trip);
    EXPECT_EQ(first.consecutiveFailures, 1U);
    EXPECT_EQ(first.suppressedSinceLastEmit, 0U);

    auto second = tracker.recordFailure(1, t0());
    auto third = tracker.recordFailure(1, t0());
    EXPECT_FALSE(second.emitDetail);
    EXPECT_FALSE(third.emitDetail);

    // Failure #4 hits the detailEmitInterval and reports the two it suppressed.
    auto fourth = tracker.recordFailure(1, t0());
    EXPECT_TRUE(fourth.emitDetail);
    EXPECT_EQ(fourth.suppressedSinceLastEmit, 2U);
}

TEST(RtpSendFailureTracker, PersistentFailureTripsAtConsecutiveThreshold) {
    RtpSendFailureTracker tracker(smallConfig());
    tracker.beginGeneration(1, t0());

    for (uint32_t i = 1; i < 10; ++i) {
        auto action = tracker.recordFailure(1, t0());
        EXPECT_FALSE(action.trip) << "tripped early at failure " << i;
    }

    auto tenth = tracker.recordFailure(1, t0());
    EXPECT_TRUE(tenth.trip);
    EXPECT_TRUE(tenth.emitDetail);
    EXPECT_EQ(tenth.consecutiveFailures, 10U);
    EXPECT_TRUE(tracker.isTripped());
}

TEST(RtpSendFailureTracker, TripEmitsExactlyOnce) {
    RtpSendFailureTracker tracker(smallConfig());
    tracker.beginGeneration(1, t0());
    for (uint32_t i = 0; i < 10; ++i) {
        (void)tracker.recordFailure(1, t0());
    }
    ASSERT_TRUE(tracker.isTripped());

    // Stale commands draining after the trip stay silent.
    auto after = tracker.recordFailure(1, t0());
    EXPECT_FALSE(after.trip);
    EXPECT_FALSE(after.emitDetail);
    auto successAfter = tracker.recordSuccess(1, t0());
    EXPECT_FALSE(successAfter.recovered);
}

TEST(RtpSendFailureTracker, SuccessAfterFailuresReportsRecoveryOnce) {
    RtpSendFailureTracker tracker(smallConfig());
    tracker.beginGeneration(1, t0());

    for (uint32_t i = 0; i < 3; ++i) {
        (void)tracker.recordFailure(1, t0());
    }
    auto recovery = tracker.recordSuccess(1, t0());
    EXPECT_TRUE(recovery.recovered);
    EXPECT_EQ(recovery.consecutiveFailures, 3U);

    auto calm = tracker.recordSuccess(1, t0());
    EXPECT_FALSE(calm.recovered);
}

TEST(RtpSendFailureTracker, IntermittentFailuresNeverTripConsecutively) {
    auto config = smallConfig();
    config.windowedFailuresToTrip = 1000; // isolate the consecutive rule
    RtpSendFailureTracker tracker(config);
    tracker.beginGeneration(1, t0());

    for (uint32_t i = 0; i < 100; ++i) {
        auto failure = tracker.recordFailure(1, t0());
        EXPECT_FALSE(failure.trip);
        auto success = tracker.recordSuccess(1, t0());
        EXPECT_TRUE(success.recovered);
    }
    EXPECT_FALSE(tracker.isTripped());
}

TEST(RtpSendFailureTracker, WindowedFlappingTripsDespiteConsecutiveResets) {
    RtpSendFailureTracker tracker(smallConfig()); // 20 windowed within 5 s
    tracker.beginGeneration(1, t0());

    // 4 failures + 1 success, repeated: the consecutive counter never exceeds
    // 4, but the window accumulates.
    bool tripped = false;
    for (uint32_t round = 0; round < 5 && !tripped; ++round) {
        for (uint32_t i = 0; i < 4; ++i) {
            tripped = tracker.recordFailure(1, t0()).trip || tripped;
        }
        (void)tracker.recordSuccess(1, t0());
    }
    EXPECT_TRUE(tripped);
    EXPECT_TRUE(tracker.isTripped());
}

TEST(RtpSendFailureTracker, OldFailuresAgeOutOfTheWindow) {
    auto config = smallConfig();             // 20 windowed within 5 s
    config.consecutiveFailuresToTrip = 1000; // isolate the windowed rule
    RtpSendFailureTracker tracker(config);
    tracker.beginGeneration(1, t0());

    for (uint32_t i = 0; i < 15; ++i) {
        EXPECT_FALSE(tracker.recordFailure(1, t0()).trip);
    }
    // Everything above ages out of the 5 s window...
    (void)tracker.recordSuccess(1, t0() + seconds(6));
    // ...so 15 more failures don't reach the windowed threshold either.
    for (uint32_t i = 0; i < 5; ++i) {
        auto action = tracker.recordFailure(1, t0() + seconds(7));
        EXPECT_FALSE(action.trip);
        EXPECT_LE(action.windowedFailures, 5U);
    }
    EXPECT_FALSE(tracker.isTripped());
}

TEST(RtpSendFailureTracker, StaleGenerationCannotContributeToNewOne) {
    RtpSendFailureTracker tracker(smallConfig());
    tracker.beginGeneration(1, t0());

    for (uint32_t i = 0; i < 9; ++i) {
        (void)tracker.recordFailure(1, t0());
    }
    ASSERT_FALSE(tracker.isTripped());

    // A failure from a different generation resets all state implicitly.
    auto crossGeneration = tracker.recordFailure(2, t0());
    EXPECT_EQ(crossGeneration.consecutiveFailures, 1U);
    EXPECT_FALSE(crossGeneration.trip);
    EXPECT_EQ(tracker.generation(), 2U);
}

TEST(RtpSendFailureTracker, BeginGenerationClearsATrip) {
    RtpSendFailureTracker tracker(smallConfig());
    tracker.beginGeneration(1, t0());
    for (uint32_t i = 0; i < 10; ++i) {
        (void)tracker.recordFailure(1, t0());
    }
    ASSERT_TRUE(tracker.isTripped());

    tracker.beginGeneration(2, t0());
    EXPECT_FALSE(tracker.isTripped());
    auto action = tracker.recordFailure(2, t0());
    EXPECT_EQ(action.consecutiveFailures, 1U);
}

TEST(RtpTerminalFailureRegistry, PublishedGenerationIsTrippedAndFindable) {
    RtpTerminalFailureRegistry registry;
    EXPECT_FALSE(registry.isTripped(7));

    TerminalFailure failure;
    failure.generation = 7;
    failure.errorMessage = "socket died";
    failure.ownerId = "session:test";
    failure.consecutiveFailures = 50;
    registry.publish(failure);

    EXPECT_TRUE(registry.isTripped(7));
    auto found = registry.find(7);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->errorMessage, "socket died");
    EXPECT_EQ(found->ownerId, "session:test");
    EXPECT_EQ(found->consecutiveFailures, 50U);
}

TEST(RtpTerminalFailureRegistry, UnknownAndZeroGenerationsAreNotTripped) {
    RtpTerminalFailureRegistry registry;
    EXPECT_FALSE(registry.isTripped(0));
    EXPECT_FALSE(registry.isTripped(42));

    TerminalFailure zeroGeneration; // generation 0 is never valid
    registry.publish(zeroGeneration);
    EXPECT_FALSE(registry.isTripped(0));
}

TEST(RtpTerminalFailureRegistry, RecordSurvivesNewerGenerations) {
    // The final-frame case: generation G trips after its lease is released,
    // and a newer playback acquires G+1 before the event loop looks. G's
    // record must still be readable.
    RtpTerminalFailureRegistry registry;

    TerminalFailure first;
    first.generation = 7;
    registry.publish(first);

    TerminalFailure second;
    second.generation = 8;
    registry.publish(second);

    EXPECT_TRUE(registry.isTripped(7));
    EXPECT_TRUE(registry.isTripped(8));
}

TEST(RtpTerminalFailureRegistry, RingEvictsOldestBeyondCapacity) {
    RtpTerminalFailureRegistry registry;
    for (uint64_t generation = 1; generation <= RtpTerminalFailureRegistry::RING_SIZE + 1; ++generation) {
        TerminalFailure failure;
        failure.generation = generation;
        registry.publish(failure);
    }

    EXPECT_FALSE(registry.isTripped(1)); // evicted
    for (uint64_t generation = 2; generation <= RtpTerminalFailureRegistry::RING_SIZE + 1; ++generation) {
        EXPECT_TRUE(registry.isTripped(generation)) << "generation " << generation;
    }
    EXPECT_FALSE(registry.find(1).has_value());
}

} // namespace creatures::rtp
