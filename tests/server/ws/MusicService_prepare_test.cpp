#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "api/MusicContracts.h"
#include "model/MusicPiece.h"
#include "server/ws/service/MusicService.h"

namespace {

constexpr const char *PIECE_ID = "00000000-0000-4000-8000-000000000010";
constexpr const char *VERSION_ID = "00000000-0000-4000-8000-000000000001";

nlohmann::json section(const char *text, int64_t durationMs, std::vector<std::string> styles) {
    return {{"text", text}, {"duration_ms", durationMs}, {"positive_styles", styles}};
}

creatures::MusicPiece pieceWithOneVersion() {
    const nlohmann::json json = {{"id", PIECE_ID},
                                 {"title", "Parrot Parade"},
                                 {"notes", ""},
                                 {"created_at", 1758240000000},
                                 {"updated_at", 1758240001000},
                                 {"current_version_id", VERSION_ID},
                                 {"versions",
                                  {{{"id", VERSION_ID},
                                    {"song_id", "XupGfbMbKaS8iJcIKm2o"},
                                    {"sound_file", "music/parrot-parade--00000000-000.wav"},
                                    {"mp3_url", "/api/v1/sound/mp3/parrot-parade--00000000-000.mp3"},
                                    {"duration_ms", 30000},
                                    {"sections",
                                     {section("[Intro]", 6000, {"pizzicato"}), section("[Verse]", 18000, {"strings"}),
                                      section("[Outro]", 6000, {"ritardando"})}},
                                    {"created_at", 1758240000000}}}}};
    return creatures::musicPieceFromJson(json).getValue().value();
}

} // namespace

/// The prod loop (3.48.1): refine said "kept [0, 2]" and generate must turn
/// exactly those into audio-refs of the base version's song — the same
/// sections through the same piece must never disagree.
TEST(MusicServicePrepare, KeptSectionsBecomeAudioRefsOfTheBaseVersionSong) {
    const auto piece = pieceWithOneVersion();
    nlohmann::json request = {{"piece_id", PIECE_ID},
                              {"base_version_id", VERSION_ID},
                              {"sections",
                               {section("[Intro]", 6000, {"pizzicato"}), section("[Verse]", 18000, {"strings", "pads"}),
                                section("[Outro]", 6000, {"ritardando"})}},
                              {"keep", {0, 2}},
                              {"seed", 21}};
    const auto parsed = creatures::api::musicGenerateRequestFromJson(request);
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();

    const auto prepared = creatures::ws::MusicService::prepare(parsed.getValue().value(), &piece);
    ASSERT_TRUE(prepared.isSuccess()) << prepared.getError()->getMessage();
    const auto ready = prepared.getValue().value();
    ASSERT_TRUE(ready.upstream.compositionPlan.has_value());
    const auto &chunks = ready.upstream.compositionPlan->chunks;
    ASSERT_EQ(chunks.size(), 3u);
    EXPECT_TRUE(chunks[0].isAudioRef());
    EXPECT_EQ(chunks[0].audioRef->songId, "XupGfbMbKaS8iJcIKm2o");
    EXPECT_EQ(chunks[0].audioRef->endMs, 6000);
    EXPECT_FALSE(chunks[1].isAudioRef());
    EXPECT_EQ(chunks[1].conditioningRef->songId, "XupGfbMbKaS8iJcIKm2o");
    EXPECT_TRUE(chunks[2].isAudioRef());
    EXPECT_EQ(chunks[2].audioRef->startMs, 24000);
    EXPECT_EQ(ready.context.title, "Parrot Parade");
    EXPECT_EQ(ready.context.baseVersionId, VERSION_ID);
    EXPECT_EQ(ready.context.pieceId, PIECE_ID);
    ASSERT_TRUE(ready.context.sections.has_value());
    EXPECT_EQ(ready.upstream.seed.value(), 21);
}

TEST(MusicServicePrepare, RejectsUnknownBaseVersionAndMissingPiece) {
    const auto piece = pieceWithOneVersion();
    nlohmann::json request = {{"piece_id", PIECE_ID},
                              {"base_version_id", "00000000-0000-4000-8000-000000000099"},
                              {"sections", {section("[Intro]", 6000, {"pizzicato"})}},
                              {"keep", {0}}};
    const auto parsed = creatures::api::musicGenerateRequestFromJson(request).getValue().value();
    const auto unknown = creatures::ws::MusicService::prepare(parsed, &piece);
    ASSERT_FALSE(unknown.isSuccess());
    EXPECT_NE(unknown.getError()->getMessage().find("has no version"), std::string::npos);
    EXPECT_FALSE(creatures::ws::MusicService::prepare(parsed, nullptr).isSuccess());
}

TEST(MusicServicePrepare, PromptModeNeedsNoPiece) {
    const auto parsed =
        creatures::api::musicGenerateRequestFromJson({{"prompt", "underscore"}, {"music_length_ms", 12000}})
            .getValue()
            .value();
    const auto prepared = creatures::ws::MusicService::prepare(parsed, nullptr);
    ASSERT_TRUE(prepared.isSuccess()) << prepared.getError()->getMessage();
    EXPECT_EQ(prepared.getValue()->upstream.prompt, "underscore");
    EXPECT_EQ(prepared.getValue()->upstream.musicLengthMs, 12000);
    EXPECT_FALSE(prepared.getValue()->context.sections.has_value());
}
