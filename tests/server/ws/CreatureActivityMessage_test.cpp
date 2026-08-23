#include "gtest/gtest.h"

#include <nlohmann/json.hpp>

#include "api/WebSocketEnvelope.h"
#include "server/runtime/Activity.h"
#include "server/runtime/RuntimeSnapshot.h"
#include "server/ws/dto/websocket/MessageTypes.h"

using namespace creatures;

TEST(CreatureActivityMessageTest, IdleStateSerialization) {
    const auto json = api::serializeWebSocketEnvelope(
        ws::toString(ws::MessageType::IdleStateChanged),
        {{"creature_id", "creature-1"}, {"idle_enabled", true}, {"timestamp", "2025-01-01T00:00:00Z"}});
    EXPECT_EQ(
        json,
        R"({"command":"idle-state-changed","payload":{"creature_id":"creature-1","idle_enabled":true,"timestamp":"2025-01-01T00:00:00Z"}})");
}

TEST(CreatureActivityMessageTest, ActivitySerialization) {
    const auto json = api::webSocketEnvelopeToJson(ws::toString(ws::MessageType::CreatureActivity),
                                                   {{"creature_id", "beaky"},
                                                    {"creature_name", "Beaky"},
                                                    {"state", runtime::toString(runtime::ActivityState::Running)},
                                                    {"animation_id", "anim-123"},
                                                    {"session_id", "uuid-1234"},
                                                    {"reason", runtime::toString(runtime::ActivityReason::AdHoc)},
                                                    {"timestamp", "2025-02-02T00:00:00Z"}});

    EXPECT_EQ(json, (nlohmann::json{{"command", "creature-activity"},
                                    {"payload",
                                     {{"creature_id", "beaky"},
                                      {"creature_name", "Beaky"},
                                      {"state", "running"},
                                      {"animation_id", "anim-123"},
                                      {"session_id", "uuid-1234"},
                                      {"reason", "ad_hoc"},
                                      {"timestamp", "2025-02-02T00:00:00Z"}}}}));
}

TEST(CreatureActivityMessageTest, RuntimeSnapshotPreservesNullableFields) {
    const runtime::CreatureRuntimeSnapshot snapshot{
        false,
        {"disabled", std::nullopt, std::nullopt, "disabled", "2025-02-02T00:00:00Z", "2025-02-02T00:00:00Z"},
        {1, 2, 3, 4, 5, 6, 7, 8},
        std::nullopt,
        std::nullopt};

    EXPECT_EQ(runtime::creatureRuntimeSnapshotToJson(snapshot),
              (nlohmann::json{{"idle_enabled", false},
                              {"activity",
                               {{"state", "disabled"},
                                {"animation_id", nullptr},
                                {"session_id", nullptr},
                                {"reason", "disabled"},
                                {"started_at", "2025-02-02T00:00:00Z"},
                                {"updated_at", "2025-02-02T00:00:00Z"}}},
                              {"counters",
                               {{"sessions_started_total", 1},
                                {"sessions_cancelled_total", 2},
                                {"idle_started_total", 3},
                                {"idle_stopped_total", 4},
                                {"idle_toggles_total", 5},
                                {"skips_missing_creature_total", 6},
                                {"bgm_takeovers_total", 7},
                                {"audio_resets_total", 8}}},
                              {"bgm_owner", nullptr},
                              {"last_error", nullptr}}));
}
