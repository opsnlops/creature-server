#include "gtest/gtest.h"

#include <optional>
#include <string>

#include "../TestGlobals.h"
#include "blockingconcurrentqueue.h"
#include "server/runtime/Activity.h"
#include "server/ws/service/CreatureService.h"
#include "util/ObservabilityManager.h"

// Ownership guard tests (issue #62): a session reporting its own end must still own the
// creature's activity, otherwise the stale write would stomp the state a newer session
// just broadcast — the exact edge the fixture binding dispatcher reacts to.

namespace {
using creatures::runtime::ActivityReason;
using creatures::runtime::ActivityState;
using creatures::ws::CreatureService;
} // namespace

class ActivityOwnershipTest : public ::testing::Test {
  protected:
    void SetUp() override {
        creatures::observability = std::make_shared<creatures::ObservabilityManager>();
        creatures::websocketOutgoingMessages = std::make_shared<moodycamel::BlockingConcurrentQueue<std::string>>();
    }

    void TearDown() override {
        creatures::observability.reset();
        creatures::websocketOutgoingMessages.reset();
    }
};

TEST_F(ActivityOwnershipTest, RunningWritesAreNeverStale) {
    EXPECT_FALSE(CreatureService::isStaleActivityWrite(ActivityState::Running, "session-b",
                                                       std::optional<std::string>{"session-a"}));
}

TEST_F(ActivityOwnershipTest, WritesWithoutSessionContextAreNeverStale) {
    EXPECT_FALSE(
        CreatureService::isStaleActivityWrite(ActivityState::Stopped, "", std::optional<std::string>{"session-a"}));
}

TEST_F(ActivityOwnershipTest, WritesWithNoCurrentOwnerAreNeverStale) {
    EXPECT_FALSE(CreatureService::isStaleActivityWrite(ActivityState::Stopped, "session-a", std::nullopt));
}

TEST_F(ActivityOwnershipTest, MatchingOwnerIsNotStale) {
    EXPECT_FALSE(CreatureService::isStaleActivityWrite(ActivityState::Stopped, "session-a",
                                                       std::optional<std::string>{"session-a"}));
}

TEST_F(ActivityOwnershipTest, MismatchedOwnerIsStaleForTerminalStates) {
    const std::optional<std::string> owner{"session-a"};
    EXPECT_TRUE(CreatureService::isStaleActivityWrite(ActivityState::Stopped, "session-b", owner));
    EXPECT_TRUE(CreatureService::isStaleActivityWrite(ActivityState::Idle, "session-b", owner));
    EXPECT_TRUE(CreatureService::isStaleActivityWrite(ActivityState::Disabled, "session-b", owner));
}

// End-to-end through setActivityState, observed via isCreatureStreaming: a stale stop
// from a different session must not clobber the owning session's running state.
TEST_F(ActivityOwnershipTest, StaleStopFromOtherSessionIsIgnored) {
    const std::string creature = "ownership-creature-1";

    CreatureService::setActivityState({creature}, "", ActivityReason::Streaming, ActivityState::Running, "session-a",
                                      nullptr);
    ASSERT_TRUE(CreatureService::isCreatureStreaming(creature));

    // A dead session's stop arrives late — it no longer owns the creature.
    CreatureService::setActivityState({creature}, "", ActivityReason::Cancelled, ActivityState::Stopped, "session-b",
                                      nullptr);
    EXPECT_TRUE(CreatureService::isCreatureStreaming(creature));

    // The owning session's stop still applies.
    CreatureService::setActivityState({creature}, "", ActivityReason::Streaming, ActivityState::Stopped, "session-a",
                                      nullptr);
    EXPECT_FALSE(CreatureService::isCreatureStreaming(creature));
}

TEST_F(ActivityOwnershipTest, TakeoverThenStaleStopFromOldSessionIsIgnored) {
    const std::string creature = "ownership-creature-2";

    CreatureService::setActivityState({creature}, "", ActivityReason::Streaming, ActivityState::Running, "session-old",
                                      nullptr);
    ASSERT_TRUE(CreatureService::isCreatureStreaming(creature));

    // A new session takes over — running writes always apply.
    CreatureService::setActivityState({creature}, "", ActivityReason::Streaming, ActivityState::Running, "session-new",
                                      nullptr);
    ASSERT_TRUE(CreatureService::isCreatureStreaming(creature));

    // The old session's cancellation lands after the takeover and must be dropped.
    CreatureService::setActivityState({creature}, "", ActivityReason::Cancelled, ActivityState::Stopped, "session-old",
                                      nullptr);
    EXPECT_TRUE(CreatureService::isCreatureStreaming(creature));
}

TEST_F(ActivityOwnershipTest, SessionlessStopAlwaysApplies) {
    const std::string creature = "ownership-creature-3";

    CreatureService::setActivityState({creature}, "", ActivityReason::Streaming, ActivityState::Running, "session-a",
                                      nullptr);
    ASSERT_TRUE(CreatureService::isCreatureStreaming(creature));

    // The streaming timeout path stops with no session context; it must not be guarded.
    CreatureService::setActivityState({creature}, "", ActivityReason::Streaming, ActivityState::Stopped, "", nullptr);
    EXPECT_FALSE(CreatureService::isCreatureStreaming(creature));
}
