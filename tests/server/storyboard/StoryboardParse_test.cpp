
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/Storyboard.h"
#include "server/database.h"

namespace creatures {

namespace {

// Two known-valid lowercase UUIDs for use as the storyboard id + tile ids.
// Not constexpr because they're string literals embedded into json fixtures.
constexpr const char *VALID_STORYBOARD_ID = "b4f1c0de-1111-2222-3333-444455556666";
constexpr const char *VALID_TILE_ID_A = "9a7c6b54-aaaa-bbbb-cccc-ddddeeeeffff";
constexpr const char *VALID_TILE_ID_B = "11111111-2222-3333-4444-555555555555";

// Build a minimal-but-valid storyboard JSON. Caller can mutate before parsing.
nlohmann::json validStoryboard() {
    return nlohmann::json{
        {"id", VALID_STORYBOARD_ID},
        {"title", "Halloween Front Porch"},
        {"notes", "Beaky greets; Mango heckles."},
        {"tiles", nlohmann::json::array({{{"id", VALID_TILE_ID_A},
                                          {"x", 0.06},
                                          {"y", 0.08},
                                          {"width", 0.26},
                                          {"height", 0.20},
                                          {"label", "Greet"},
                                          {"sf_symbol", "hand.wave.fill"},
                                          {"tint_color_hex", "#34C759"},
                                          {"action",
                                           {{"type", "ad_hoc_speech"},
                                            {"creature_id", "e93b9a7a-1704-11ef-84b9-3b37dddeb225"},
                                            {"resume_playlist", true}}}}})},
        {"created_at", 1748579999000},
        {"updated_at", 1748580015000},
    };
}

} // namespace

TEST(StoryboardParse, MinimalValidStoryboard) {
    auto result = Database::parseStoryboardJson(validStoryboard(), nullptr);
    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    const auto sb = result.getValue().value();
    EXPECT_EQ(sb.id, VALID_STORYBOARD_ID);
    EXPECT_EQ(sb.title, "Halloween Front Porch");
    EXPECT_EQ(sb.tiles.size(), 1u);
    EXPECT_EQ(sb.created_at, 1748579999000);
}

TEST(StoryboardParse, RejectsMissingTitle) {
    auto json = validStoryboard();
    json.erase("title");
    auto result = Database::parseStoryboardJson(json, nullptr);
    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(result.getError()->getCode(), ServerError::InvalidData);
}

TEST(StoryboardParse, RejectsEmptyTitle) {
    auto json = validStoryboard();
    json["title"] = "";
    auto result = Database::parseStoryboardJson(json, nullptr);
    ASSERT_FALSE(result.isSuccess());
}

TEST(StoryboardParse, RejectsOversizedTitle) {
    auto json = validStoryboard();
    json["title"] = std::string(MAX_STORYBOARD_TITLE + 1, 'x');
    auto result = Database::parseStoryboardJson(json, nullptr);
    ASSERT_FALSE(result.isSuccess());
}

TEST(StoryboardParse, RejectsOversizedNotes) {
    auto json = validStoryboard();
    json["notes"] = std::string(MAX_STORYBOARD_NOTES + 1, 'x');
    auto result = Database::parseStoryboardJson(json, nullptr);
    ASSERT_FALSE(result.isSuccess());
}

TEST(StoryboardParse, EmptyTilesAllowed) {
    // Per contract: tiles may be empty during authoring; empty is valid.
    auto json = validStoryboard();
    json["tiles"] = nlohmann::json::array();
    auto result = Database::parseStoryboardJson(json, nullptr);
    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    EXPECT_EQ(result.getValue().value().tiles.size(), 0u);
}

TEST(StoryboardParse, RejectsTooManyTiles) {
    auto json = validStoryboard();
    nlohmann::json tiles = nlohmann::json::array();
    // Each tile must itself be valid (UUID id) since the parser walks all
    // entries before failing the cap check would even matter.
    for (std::size_t i = 0; i < MAX_STORYBOARD_TILES + 1; ++i) {
        tiles.push_back({{"id", "00000000-0000-0000-0000-000000000000"}});
    }
    json["tiles"] = tiles;
    auto result = Database::parseStoryboardJson(json, nullptr);
    ASSERT_FALSE(result.isSuccess());
}

TEST(StoryboardParse, RejectsTileWithoutId) {
    auto json = validStoryboard();
    json["tiles"][0].erase("id");
    auto result = Database::parseStoryboardJson(json, nullptr);
    ASSERT_FALSE(result.isSuccess());
}

TEST(StoryboardParse, RejectsNonUuidTileId) {
    auto json = validStoryboard();
    json["tiles"][0]["id"] = "not-a-uuid";
    auto result = Database::parseStoryboardJson(json, nullptr);
    ASSERT_FALSE(result.isSuccess());
}

TEST(StoryboardParse, RejectsNonUuidStoryboardId) {
    auto json = validStoryboard();
    json["id"] = "definitely-not-uuid-shaped";
    auto result = Database::parseStoryboardJson(json, nullptr);
    ASSERT_FALSE(result.isSuccess());
}

TEST(StoryboardParse, RejectsOversizedTileLabel) {
    auto json = validStoryboard();
    json["tiles"][0]["label"] = std::string(MAX_STORYBOARD_TILE_LABEL + 1, 'l');
    auto result = Database::parseStoryboardJson(json, nullptr);
    ASSERT_FALSE(result.isSuccess());
}

TEST(StoryboardParse, RejectsNonObjectAction) {
    auto json = validStoryboard();
    json["tiles"][0]["action"] = "this-should-be-an-object";
    auto result = Database::parseStoryboardJson(json, nullptr);
    ASSERT_FALSE(result.isSuccess());
}

// This is the load-bearing forward-compat guarantee. The server must round-
// trip unknown action `type` values + unknown nested keys verbatim so old/new
// clients interoperate as the client's action vocabulary grows.
TEST(StoryboardParse, OpaqueActionRoundTrip) {
    auto json = validStoryboard();
    // Use an action type the server has never heard of, plus arbitrary nested
    // structure under it. If the parser strips ANY of this, the test fails.
    nlohmann::json futureAction = {
        {"type", "future_action_xyz"},
        {"new_string_param", "with-special-characters: 🥭/ñ/\"quoted\""},
        {"new_int_param", 42},
        {"new_nested",
         {{"creature_id", "e93b9a7a-1704-11ef-84b9-3b37dddeb225"},
          {"options", {{"strength", 0.7}, {"loop", false}, {"tags", {"a", "b", "c"}}}}}},
        {"new_array", {1, 2, 3, 4}},
    };
    json["tiles"].push_back({
        {"id", VALID_TILE_ID_B},
        {"x", 0.40},
        {"y", 0.40},
        {"width", 0.26},
        {"height", 0.20},
        {"label", "Future"},
        {"action", futureAction},
    });

    auto parsed = Database::parseStoryboardJson(json, nullptr);
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();

    auto roundTripped = storyboardToJson(parsed.getValue().value());

    // The action on tile index 1 must be byte-equal (deep equality) to what
    // we sent — every field preserved, including types the server doesn't
    // understand. This is the regression check for issue forward-compat.
    EXPECT_EQ(roundTripped["tiles"][1]["action"], futureAction);

    // And the rest of the tile should also survive (id, label, geometry).
    EXPECT_EQ(roundTripped["tiles"][1]["id"], VALID_TILE_ID_B);
    EXPECT_EQ(roundTripped["tiles"][1]["label"], "Future");
    EXPECT_DOUBLE_EQ(roundTripped["tiles"][1]["x"].get<double>(), 0.40);
}

} // namespace creatures
