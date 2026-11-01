#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include <nlohmann/json.hpp>

#include "server/transport/CreatureReadHandlers.h"

namespace creatures::transport {
namespace {

constexpr const char *CREATURE_ID = "11111111-2222-4333-8444-555555555555";

api::CreatureResponse creature() {
    Creature value{};
    value.id = CREATURE_ID;
    value.name = "Beaky";
    value.channel_offset = 12;
    value.audio_channel = 1;
    value.mouth_slot = 4;
    return {value, {}};
}

TEST(CreatureReadHandlers, ListsCreaturesUsingTheCanonicalEnvelope) {
    const auto response = listCreatures(nullptr, [](const auto &) {
        return Result<std::vector<api::CreatureResponse>>{std::vector<api::CreatureResponse>{creature()}};
    });

    ASSERT_EQ(response.statusCode, 200);
    ASSERT_EQ(response.contentType, "application/json; charset=utf-8");
    const auto body = nlohmann::json::parse(response.body);
    EXPECT_EQ(body.at("count"), 1);
    EXPECT_EQ(body.at("items").at(0).at("id"), CREATURE_ID);
    EXPECT_TRUE(body.at("items").at(0).contains("runtime"));
}

TEST(CreatureReadHandlers, RejectsAnInvalidCreatureIdBeforeCallingTheService) {
    std::size_t calls = 0;
    const auto response = getCreature("not-a-uuid", nullptr, [&calls](const auto &, const auto &) {
        ++calls;
        return Result<api::CreatureResponse>{creature()};
    });

    EXPECT_EQ(calls, 0);
    EXPECT_EQ(response.statusCode, 400);
    const auto body = nlohmann::json::parse(response.body);
    EXPECT_EQ(body.at("status"), "error");
    EXPECT_EQ(body.at("message"), "creatureId must be a UUID");
}

TEST(CreatureReadHandlers, MapsServiceNotFoundToTheCanonicalEnvelope) {
    const auto response = getCreature(CREATURE_ID, nullptr, [](const auto &, const auto &) {
        return Result<api::CreatureResponse>{ServerError(ServerError::NotFound, "Creature not found")};
    });

    EXPECT_EQ(response.statusCode, 404);
    const auto body = nlohmann::json::parse(response.body);
    EXPECT_EQ(body.at("status"), "not_found");
    EXPECT_EQ(body.at("code"), 404);
}

TEST(CreatureReadHandlers, ReturnsOneCreatureWithRuntimeState) {
    const auto response = getCreature(
        CREATURE_ID, nullptr, [](const auto &, const auto &) { return Result<api::CreatureResponse>{creature()}; });

    EXPECT_EQ(response.statusCode, 200);
    const auto body = nlohmann::json::parse(response.body);
    EXPECT_EQ(body.at("id"), CREATURE_ID);
    EXPECT_EQ(body.at("name"), "Beaky");
    EXPECT_TRUE(body.contains("runtime"));
}

} // namespace
} // namespace creatures::transport
