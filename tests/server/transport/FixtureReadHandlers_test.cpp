#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <vector>

#include <nlohmann/json.hpp>

#include "server/transport/FixtureReadHandlers.h"

namespace creatures::transport {
namespace {

constexpr const char *FIXTURE_ID = "11111111-2222-4333-8444-555555555555";

DmxFixture fixture() {
    DmxFixture value;
    value.id = FIXTURE_ID;
    value.name = "Stage Left Spot";
    value.type = FixtureType::Light;
    value.channel_offset = 12;
    value.assigned_universe = 1;
    value.channels = {{.offset = 0, .name = "red", .kind = "color_red"}};
    return value;
}

TEST(FixtureReadHandlers, ListsFixturesUsingTheCanonicalEnvelope) {
    const auto response = listFixtures(
        nullptr, [](const auto &) { return Result<std::vector<DmxFixture>>{std::vector<DmxFixture>{fixture()}}; });

    ASSERT_EQ(response.statusCode, 200);
    ASSERT_EQ(response.contentType, "application/json; charset=utf-8");
    const auto body = nlohmann::json::parse(response.body);
    EXPECT_EQ(body.at("count"), 1);
    EXPECT_EQ(body.at("items").at(0).at("id"), FIXTURE_ID);
    EXPECT_EQ(body.at("items").at(0).at("assigned_universe"), 1);
}

TEST(FixtureReadHandlers, RejectsAnInvalidFixtureIdBeforeCallingTheService) {
    std::size_t calls = 0;
    const auto response = getFixture("not-a-uuid", nullptr, [&calls](const auto &, const auto &) {
        ++calls;
        return Result<DmxFixture>{fixture()};
    });

    EXPECT_EQ(calls, 0);
    EXPECT_EQ(response.statusCode, 400);
    const auto body = nlohmann::json::parse(response.body);
    EXPECT_EQ(body.at("status"), "error");
    EXPECT_EQ(body.at("message"), "fixtureId must be a UUID");
}

TEST(FixtureReadHandlers, MapsServiceNotFoundToTheCanonicalEnvelope) {
    const auto response = getFixture(FIXTURE_ID, nullptr, [](const auto &, const auto &) {
        return Result<DmxFixture>{ServerError(ServerError::NotFound, "Fixture not found")};
    });

    EXPECT_EQ(response.statusCode, 404);
    const auto body = nlohmann::json::parse(response.body);
    EXPECT_EQ(body.at("status"), "not_found");
    EXPECT_EQ(body.at("code"), 404);
}

TEST(FixtureReadHandlers, ReturnsOneFixture) {
    const auto response =
        getFixture(FIXTURE_ID, nullptr, [](const auto &, const auto &) { return Result<DmxFixture>{fixture()}; });

    EXPECT_EQ(response.statusCode, 200);
    const auto body = nlohmann::json::parse(response.body);
    EXPECT_EQ(body.at("id"), FIXTURE_ID);
    EXPECT_EQ(body.at("name"), "Stage Left Spot");
}

} // namespace
} // namespace creatures::transport
