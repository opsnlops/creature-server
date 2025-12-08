#include "gtest/gtest.h"

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "server/runtime/Activity.h"
#include "server/ws/dto/websocket/CreatureActivityMessage.h"
#include "server/ws/dto/websocket/MessageTypes.h"

using namespace creatures::ws;

TEST(CreatureActivityMessageTest, IdleStateSerialization) {
    auto mapper = oatpp::parser::json::mapping::ObjectMapper::createShared();

    auto msg = IdleStateChangedMessage::createShared();
    msg->command = toString(MessageType::IdleStateChanged).c_str();
    auto payload = IdleStateChangedDto::createShared();
    payload->creature_id = "creature-1";
    payload->idle_enabled = true;
    payload->timestamp = "2025-01-01T00:00:00Z";
    msg->payload = payload;

    auto json = mapper->writeToString(msg);
    EXPECT_EQ(
        json,
        R"({"command":"idle-state-changed","payload":{"creature_id":"creature-1","idle_enabled":true,"timestamp":"2025-01-01T00:00:00Z"}})");
}

TEST(CreatureActivityMessageTest, ActivitySerialization) {
    auto mapper = oatpp::parser::json::mapping::ObjectMapper::createShared();

    auto msg = CreatureActivityMessage::createShared();
    msg->command = toString(MessageType::CreatureActivity).c_str();
    auto payload = CreatureActivityDto::createShared();
    payload->creature_id = "beaky";
    payload->state = creatures::runtime::toString(creatures::runtime::ActivityState::Running);
    payload->animation_id = "anim-123";
    payload->session_id = "uuid-1234";
    payload->reason = creatures::runtime::toString(creatures::runtime::ActivityReason::AdHoc);
    payload->timestamp = "2025-02-02T00:00:00Z";
    msg->payload = payload;

    auto json = mapper->writeToString(msg);
    auto parsed = mapper->readFromString<oatpp::Object<CreatureActivityMessage>>(json);
    ASSERT_TRUE(parsed);
    ASSERT_TRUE(parsed->payload);
    EXPECT_EQ(parsed->command, toString(MessageType::CreatureActivity));
    EXPECT_EQ(parsed->payload->creature_id, "beaky");
    EXPECT_EQ(parsed->payload->state, creatures::runtime::toString(creatures::runtime::ActivityState::Running));
    EXPECT_EQ(parsed->payload->animation_id, "anim-123");
    EXPECT_EQ(parsed->payload->session_id, "uuid-1234");
    EXPECT_EQ(parsed->payload->reason, creatures::runtime::toString(creatures::runtime::ActivityReason::AdHoc));
    EXPECT_EQ(parsed->payload->timestamp, "2025-02-02T00:00:00Z");
}
