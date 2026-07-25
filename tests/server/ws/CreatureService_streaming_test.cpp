#include "gtest/gtest.h"

#include <string>

#include "../TestGlobals.h"
#include "blockingconcurrentqueue.h"
#include "server/runtime/Activity.h"
#include "server/ws/service/CreatureService.h"
#include "util/ObservabilityManager.h"

namespace {
const std::string STREAMING_CREATURE = "streaming-creature";
}

class CreatureServiceStreamingTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Reset globals used by CreatureService
        creatures::observability = std::make_shared<creatures::ObservabilityManager>();
        creatures::websocketOutgoingMessages = std::make_shared<moodycamel::BlockingConcurrentQueue<std::string>>();
    }

    void TearDown() override {
        creatures::observability.reset();
        creatures::websocketOutgoingMessages.reset();
    }
};

TEST_F(CreatureServiceStreamingTest, DetectsStreamingActivity) {
    auto sessionId = creatures::ws::CreatureService::setActivityState(
        {STREAMING_CREATURE}, "" /*animationId*/, creatures::runtime::ActivityReason::Streaming,
        creatures::runtime::ActivityState::Running, "session-123", nullptr);
    EXPECT_FALSE(sessionId.empty());
    EXPECT_TRUE(creatures::ws::CreatureService::isCreatureStreaming(STREAMING_CREATURE));

    creatures::ws::CreatureService::setActivityState(
        {STREAMING_CREATURE}, "" /*animationId*/, creatures::runtime::ActivityReason::Streaming,
        creatures::runtime::ActivityState::Stopped, "session-123", nullptr);
    EXPECT_FALSE(creatures::ws::CreatureService::isCreatureStreaming(STREAMING_CREATURE));
}

// The streaming registry is set on the first frame and cleared only when streaming
// genuinely ends (issue #74). markStreamingIfNew reports "new" exactly once.
TEST_F(CreatureServiceStreamingTest, RegistryMarksOnceAndClears) {
    const std::string creature = "registry-creature";

    EXPECT_FALSE(creatures::ws::CreatureService::isCreatureStreaming(creature));

    EXPECT_TRUE(creatures::ws::CreatureService::markStreamingIfNew(creature));
    EXPECT_FALSE(creatures::ws::CreatureService::markStreamingIfNew(creature));
    EXPECT_TRUE(creatures::ws::CreatureService::isCreatureStreaming(creature));

    creatures::ws::CreatureService::clearStreaming(creature);
    EXPECT_FALSE(creatures::ws::CreatureService::isCreatureStreaming(creature));
}

// The registry must survive a transient "stopped" activity write — this is exactly the
// churn window from issue #73, where a false timeout wrote stopped while frames were
// still flowing. The schedulers trust isCreatureStreaming, so it must stay true until
// the registry itself is cleared.
TEST_F(CreatureServiceStreamingTest, RegistryOutlivesTransientStoppedActivity) {
    const std::string creature = "registry-churn-creature";

    EXPECT_TRUE(creatures::ws::CreatureService::markStreamingIfNew(creature));
    creatures::ws::CreatureService::setActivityState(
        {creature}, "" /*animationId*/, creatures::runtime::ActivityReason::Streaming,
        creatures::runtime::ActivityState::Stopped, "" /*sessionId*/, nullptr);

    EXPECT_TRUE(creatures::ws::CreatureService::isCreatureStreaming(creature));

    creatures::ws::CreatureService::clearStreaming(creature);
    EXPECT_FALSE(creatures::ws::CreatureService::isCreatureStreaming(creature));
}
