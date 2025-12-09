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
