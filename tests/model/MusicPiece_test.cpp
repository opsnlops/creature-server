#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/MusicPiece.h"

using creatures::musicPieceFromJson;
using creatures::musicPieceToJson;
using creatures::musicSectionsFromJson;

namespace {

nlohmann::json version(const char *id, int64_t durationMs) {
    return {{"id", id},
            {"song_id", "song-1"},
            {"sound_file", "music/algorithm-divine--1111aaaa2222.wav"},
            {"mp3_url", "/api/v1/sound/mp3/algorithm-divine--1111aaaa2222.mp3"},
            {"duration_ms", durationMs},
            {"recipe", {{"model_id", "music_v2_5"}, {"request_kind", "composition_plan"}}},
            {"sections", {{{"text", "[Intro]"}, {"duration_ms", durationMs}, {"positive_styles", {"strings"}}}}},
            {"created_at", 1758240000000}};
}

nlohmann::json piece() {
    return {{"id", "00000000-0000-4000-8000-000000000010"},
            {"title", "Algorithm Divine"},
            {"notes", ""},
            {"created_at", 1758240000000},
            {"updated_at", 1758240001000},
            {"current_version_id", "00000000-0000-4000-8000-000000000002"},
            {"versions",
             {version("00000000-0000-4000-8000-000000000001", 8000),
              version("00000000-0000-4000-8000-000000000002", 9000)}}};
}

} // namespace

TEST(MusicPiece, RoundTripsAStoredDocument) {
    auto stored = piece();
    stored["versions"][1]["base_version_id"] = "00000000-0000-4000-8000-000000000001";
    stored["versions"][1]["source_dialog"] = {{"script_id", "00000000-0000-4000-8000-000000000020"},
                                              {"dialog_cache_key", std::string(64, 'a')},
                                              {"dialog_generation_id", "00000000-0000-4000-8000-000000000021"}};
    const auto parsed = musicPieceFromJson(stored);
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    const auto p = parsed.getValue().value();
    EXPECT_EQ(p.title, "Algorithm Divine");
    ASSERT_EQ(p.versions.size(), 2u);
    EXPECT_EQ(p.currentVersion()->id, "00000000-0000-4000-8000-000000000002");
    EXPECT_EQ(p.versions[1].base_version_id, "00000000-0000-4000-8000-000000000001");
    ASSERT_TRUE(p.versions[1].source_dialog.has_value());
    EXPECT_EQ(p.versions[1].sections[0].contextAdherence, "high"); // default
    EXPECT_TRUE(p.versions[0].sections[0].negativeStyles.empty());

    const auto again = musicPieceFromJson(musicPieceToJson(p));
    ASSERT_TRUE(again.isSuccess()) << again.getError()->getMessage();
    EXPECT_EQ(again.getValue().value(), p);
    // Absent optionals stay absent on output.
    const auto json = musicPieceToJson(p);
    EXPECT_FALSE(json["versions"][0].contains("base_version_id"));
    EXPECT_FALSE(json["versions"][0].contains("source_dialog"));
}

TEST(MusicPiece, RejectsBrokenDocuments) {
    auto j = piece();
    j["current_version_id"] = "00000000-0000-4000-8000-000000000099";
    EXPECT_NE(musicPieceFromJson(j).getError()->getMessage().find("does not name a version"), std::string::npos);

    j = piece();
    j["versions"][1]["id"] = j["versions"][0]["id"];
    EXPECT_NE(musicPieceFromJson(j).getError()->getMessage().find("duplicate id"), std::string::npos);

    j = piece();
    j["versions"][0].erase("sections");
    EXPECT_NE(musicPieceFromJson(j).getError()->getMessage().find("versions[0].sections is required"),
              std::string::npos);

    j = piece();
    j["versions"] = nlohmann::json::array();
    EXPECT_FALSE(musicPieceFromJson(j).isSuccess());

    j = piece();
    j["mystery"] = 1;
    EXPECT_FALSE(musicPieceFromJson(j).isSuccess());
}

TEST(MusicPiece, SectionsParserAppliesChunkLimitsWithPaths) {
    auto ok =
        musicSectionsFromJson(nlohmann::json::parse(R"([{"text":"[A]","duration_ms":3000,"positive_styles":["x"]}])"),
                              "save request.sections");
    ASSERT_TRUE(ok.isSuccess()) << ok.getError()->getMessage();

    auto tooShort = musicSectionsFromJson(
        nlohmann::json::parse(R"([{"text":"[A]","duration_ms":2999,"positive_styles":["x"]}])"), "req.sections");
    EXPECT_NE(tooShort.getError()->getMessage().find("req.sections[0].duration_ms"), std::string::npos);

    auto audioRef = musicSectionsFromJson(
        nlohmann::json::parse(R"([{"song_id":"s","range":{"start_ms":0,"end_ms":5000}}])"), "req.sections");
    EXPECT_FALSE(audioRef.isSuccess()); // sections are editable content, never references

    auto conditioned = musicSectionsFromJson(
        nlohmann::json::parse(
            R"([{"text":"[A]","duration_ms":5000,"positive_styles":["x"],"conditioning_ref":{"song_id":"s","range":{"start_ms":0,"end_ms":5000}}}])"),
        "req.sections");
    EXPECT_FALSE(conditioned.isSuccess());

    EXPECT_FALSE(musicSectionsFromJson(nlohmann::json::array(), "req.sections").isSuccess());
}
