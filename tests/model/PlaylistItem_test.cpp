#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/PlaylistItem.h"
#include "server/ws/dto/PlaylistItemDto.h"

namespace creatures {

namespace {

constexpr auto ANIMATION_ID = "8e3a4b5c-1d2f-4e6a-9b0c-7f8e9d0a1b2c";

nlohmann::json validPlaylistItemJson() { return {{"animation_id", ANIMATION_ID}, {"weight", 25}}; }

void expectInvalid(const nlohmann::json &json, const std::string &messagePart) {
    const auto result = playlistItemFromJson(json, "playlist.items[3]");
    ASSERT_FALSE(result.isSuccess());
    ASSERT_TRUE(result.getError().has_value());
    EXPECT_NE(result.getError()->getMessage().find(messagePart), std::string::npos) << result.getError()->getMessage();
}

} // namespace

TEST(PlaylistItemJson, SerializesExactCanonicalShape) {
    const PlaylistItem item{ANIMATION_ID, 25};
    EXPECT_EQ(playlistItemToJson(item), validPlaylistItemJson());
}

TEST(PlaylistItemJson, AcceptsMaximumContractWeight) {
    auto json = validPlaylistItemJson();
    json["weight"] = MAX_PLAYLIST_ITEM_WEIGHT;

    const auto result = playlistItemFromJson(json);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.getValue().value(), (PlaylistItem{ANIMATION_ID, MAX_PLAYLIST_ITEM_WEIGHT}));
}

TEST(PlaylistItemJson, RoundTripsThroughNeutralCodec) {
    const PlaylistItem item{ANIMATION_ID, 7};
    const auto result = playlistItemFromJson(playlistItemToJson(item));
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.getValue().value(), item);
}

TEST(PlaylistItemJson, RejectsMissingNullAndWrongTypeFields) {
    expectInvalid(nlohmann::json::array(), "playlist.items[3] must be an object");

    for (const auto *field : {"animation_id", "weight"}) {
        auto missing = validPlaylistItemJson();
        missing.erase(field);
        expectInvalid(missing, std::string(".") + field + " is required");

        auto nullValue = validPlaylistItemJson();
        nullValue[field] = nullptr;
        expectInvalid(nullValue, std::string(".") + field + " must");
    }

    auto wrongId = validPlaylistItemJson();
    wrongId["animation_id"] = 12;
    expectInvalid(wrongId, ".animation_id must be a string");

    for (const auto &wrongWeight : {nlohmann::json("1"), nlohmann::json(true), nlohmann::json(1.5)}) {
        auto json = validPlaylistItemJson();
        json["weight"] = wrongWeight;
        expectInvalid(json, ".weight must be an integer");
    }
}

TEST(PlaylistItemJson, RejectsInvalidValuesAndUnknownFields) {
    auto emptyId = validPlaylistItemJson();
    emptyId["animation_id"] = "";
    expectInvalid(emptyId, ".animation_id must not be empty");

    auto malformedId = validPlaylistItemJson();
    malformedId["animation_id"] = "not-an-animation-uuid";
    expectInvalid(malformedId, ".animation_id must be a UUID");

    auto oversizedId = validPlaylistItemJson();
    oversizedId["animation_id"] = std::string(37, 'a');
    expectInvalid(oversizedId, ".animation_id is 37 bytes; maximum is 36");

    auto zeroWeight = validPlaylistItemJson();
    zeroWeight["weight"] = 0;
    expectInvalid(zeroWeight, ".weight must be greater than zero");

    auto negativeWeight = validPlaylistItemJson();
    negativeWeight["weight"] = -1;
    expectInvalid(negativeWeight, ".weight must not be negative");

    auto excessiveWeight = validPlaylistItemJson();
    excessiveWeight["weight"] = MAX_PLAYLIST_ITEM_WEIGHT + 1;
    expectInvalid(excessiveWeight, ".weight exceeds maximum 999");

    auto unknown = validPlaylistItemJson();
    unknown["future_field"] = true;
    expectInvalid(unknown, "contains unknown field 'future_field'");
}

TEST(PlaylistItemLegacyDto, AdaptsWithoutChangingTheNeutralModel) {
    const PlaylistItem item{ANIMATION_ID, 12};
    const auto dto = convertToDto(item);

    ASSERT_TRUE(dto);
    EXPECT_EQ(std::string(dto->animation_id), item.animation_id);
    EXPECT_EQ(*dto->weight, item.weight);
    EXPECT_EQ(convertFromDto(dto), item);
}

} // namespace creatures
