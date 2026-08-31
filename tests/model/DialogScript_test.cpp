#include <gtest/gtest.h>

#include "model/DialogScript.h"

TEST(DialogScript, BackgroundMusicSerializesToCanonicalJson) {
    creatures::DialogScript script;
    script.id = "7aaf11a0-d75d-4ca2-891d-9f3493dc66e4";
    script.title = "Moonlit argument";
    script.turns = {{"55a2af23-e797-462b-8c91-e0bc23b86fd4", "I knew you'd come back."}};
    script.background_music =
        creatures::DialogBackgroundMusic{"dialog/music/moonlit-argument--bgm--uneasy-strings--0123456789ab.wav",
                                         "01234567-89ab-4def-8123-456789abcdef",
                                         "uneasy chamber strings, restrained and instrumental",
                                         123456789,
                                         "",
                                         ""};

    const auto json = creatures::dialogScriptToJson(script);
    EXPECT_EQ(json["background_music"]["sound_file"], script.background_music->sound_file);
    EXPECT_EQ(json["background_music"]["generation_id"], script.background_music->generation_id);
    EXPECT_EQ(json["background_music"]["prompt"], script.background_music->prompt);
    EXPECT_EQ(json["background_music"]["accepted_at"], script.background_music->accepted_at);
}

TEST(DialogScript, ClearingBackgroundMusicOmitsReferenceFromJson) {
    creatures::DialogScript script;
    script.id = "7aaf11a0-d75d-4ca2-891d-9f3493dc66e4";
    script.title = "Moonlit argument";
    script.turns = {{"55a2af23-e797-462b-8c91-e0bc23b86fd4", "I knew you'd come back."}};
    script.background_music =
        creatures::DialogBackgroundMusic{"dialog/music/moonlit-argument--bgm--uneasy-strings--0123456789ab.wav",
                                         "01234567-89ab-4def-8123-456789abcdef",
                                         "uneasy chamber strings, restrained and instrumental",
                                         123456789,
                                         "",
                                         ""};

    script.background_music.reset();

    const auto json = creatures::dialogScriptToJson(script);
    EXPECT_FALSE(json.contains("background_music"));
}

// ===========================================================================
// stage_id round-trip (issue #123)
//
// stage_id was added to the struct, the JSON parser, dialogScriptToJson and
// the DTO — but not to convertToDto, so every read route returned null. The
// value was persisted correctly the whole time; it was dropped on the way out.
//
// Every field on this model needs to survive the canonical JSON response.
// ===========================================================================

TEST(DialogScriptStageBindingTest, BoundScriptIncludesStageId) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "bound scene";
    script.stage_id = "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329";

    const auto json = creatures::dialogScriptToJson(script);
    ASSERT_TRUE(json.contains("stage_id"));
    EXPECT_EQ(json["stage_id"], "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329");
}

TEST(DialogScriptStageBindingTest, UnboundScriptOmitsStageIdRatherThanSendingEmpty) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "unbound scene";

    EXPECT_FALSE(creatures::dialogScriptToJson(script).contains("stage_id"))
        << "an unbound script should omit stage_id, not send an empty string";
}

TEST(DialogScriptStageBindingTest, StageIdSurvivesTheJsonRoundTrip) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "bound scene";
    script.stage_id = "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329";

    const auto json = creatures::dialogScriptToJson(script);
    ASSERT_TRUE(json.contains("stage_id"));
    EXPECT_EQ(json["stage_id"].get<std::string>(), script.stage_id);

    creatures::DialogScript unbound;
    unbound.id = script.id;
    unbound.title = "unbound";
    EXPECT_FALSE(creatures::dialogScriptToJson(unbound).contains("stage_id"));
}

// ===========================================================================
// Accepted voice take (issue #131)
//
// The canonical JSON response is the HTTP and persistence shape.
// ===========================================================================

namespace {

creatures::AcceptedVoice sampleAcceptedVoice() {
    creatures::AcceptedVoice v;
    v.generation_id = "9f1fd726-3b78-4b4a-b9aa-fdf7f4a62329";
    v.dialog_cache_key = std::string(64, 'a');
    v.sound_file = "dialog/voice/beaky-loves-magic-9f1fd726.wav";
    v.accepted_at = 1786000000000;
    return v;
}

} // namespace

TEST(AcceptedVoiceTest, SerializesEveryField) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "scene";
    script.accepted_voice = sampleAcceptedVoice();

    const auto json = creatures::dialogScriptToJson(script);
    ASSERT_TRUE(json.contains("accepted_voice"));
    EXPECT_EQ(json["accepted_voice"]["generation_id"], sampleAcceptedVoice().generation_id);
    EXPECT_EQ(json["accepted_voice"]["dialog_cache_key"], sampleAcceptedVoice().dialog_cache_key);
    EXPECT_EQ(json["accepted_voice"]["sound_file"], sampleAcceptedVoice().sound_file);
    EXPECT_EQ(json["accepted_voice"]["accepted_at"], 1786000000000);
}

TEST(AcceptedVoiceTest, SurvivesTheJsonRoundTrip) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "scene";
    script.accepted_voice = sampleAcceptedVoice();

    const auto json = creatures::dialogScriptToJson(script);
    ASSERT_TRUE(json.contains("accepted_voice"));
    EXPECT_EQ(json["accepted_voice"]["generation_id"].get<std::string>(), sampleAcceptedVoice().generation_id);
    EXPECT_EQ(json["accepted_voice"]["dialog_cache_key"].get<std::string>(), sampleAcceptedVoice().dialog_cache_key);
    EXPECT_EQ(json["accepted_voice"]["sound_file"].get<std::string>(), sampleAcceptedVoice().sound_file);
    EXPECT_EQ(json["accepted_voice"]["accepted_at"].get<int64_t>(), 1786000000000);
}

TEST(AcceptedVoiceTest, ScriptWithNoAcceptanceOmitsTheFieldEntirely) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "unaccepted";

    EXPECT_FALSE(creatures::dialogScriptToJson(script).contains("accepted_voice"));
}

// ===========================================================================
// Music composition source (issue #136)
//
// Music is fitted to one voice take's timing and length. Without recording
// which take, the Console can flag stale *candidates* but the already-accepted
// card stays green — describing audio that will never render again. These pin
// the complete canonical JSON response.
// ===========================================================================

namespace {

creatures::DialogScript scriptWithMusic() {
    creatures::DialogScript script;
    script.id = "7aaf11a0-d75d-4ca2-891d-9f3493dc66e4";
    script.title = "Moonlit argument";
    script.turns = {{"55a2af23-e797-462b-8c91-e0bc23b86fd4", "I knew you'd come back."}};
    script.background_music =
        creatures::DialogBackgroundMusic{"dialog/music/moonlit-argument--bgm--uneasy-strings--0123456789ab.wav",
                                         "01234567-89ab-4def-8123-456789abcdef",
                                         "uneasy chamber strings, restrained and instrumental",
                                         123456789,
                                         "9f8e7d6c-5b4a-4938-8271-6f5e4d3c2b1a",
                                         std::string(64, 'a')};
    return script;
}

} // namespace

TEST(MusicCompositionSource, SurvivesTheJsonRoundTrip) {
    const auto script = scriptWithMusic();
    const auto json = creatures::dialogScriptToJson(script);
    EXPECT_EQ(json["background_music"]["source_dialog_generation_id"],
              script.background_music->source_dialog_generation_id);
    EXPECT_EQ(json["background_music"]["source_dialog_cache_key"], script.background_music->source_dialog_cache_key);
}

TEST(MusicCompositionSource, SerializerIncludesSourceFields) {
    const auto script = scriptWithMusic();
    const auto json = creatures::dialogScriptToJson(script);
    EXPECT_EQ(json["background_music"]["source_dialog_generation_id"],
              script.background_music->source_dialog_generation_id);
    EXPECT_EQ(json["background_music"]["source_dialog_cache_key"], script.background_music->source_dialog_cache_key);
}

TEST(MusicCompositionSource, MusicWithoutASourceOmitsTheFieldsRatherThanSendingThemEmpty) {
    // An unrecorded source must be distinguishable from a known-empty one, so
    // the Console shows "no verdict" instead of a passing one. Music accepted
    // before #136 lands is in exactly this state.
    auto script = scriptWithMusic();
    script.background_music->source_dialog_generation_id.clear();
    script.background_music->source_dialog_cache_key.clear();

    const auto json = creatures::dialogScriptToJson(script);
    EXPECT_FALSE(json["background_music"].contains("source_dialog_generation_id"));
    EXPECT_FALSE(json["background_music"].contains("source_dialog_cache_key"));
}
