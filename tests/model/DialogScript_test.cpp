#include <gtest/gtest.h>

#include "model/DialogScript.h"
#include "server/ws/dto/DialogScriptDto.h"

TEST(DialogScript, BackgroundMusicRoundTripsThroughJsonAndDto) {
    creatures::DialogScript script;
    script.id = "7aaf11a0-d75d-4ca2-891d-9f3493dc66e4";
    script.title = "Moonlit argument";
    script.turns = {{"55a2af23-e797-462b-8c91-e0bc23b86fd4", "I knew you'd come back."}};
    script.background_music = creatures::DialogBackgroundMusic{
        "dialog/music/moonlit-argument--bgm--uneasy-strings--0123456789ab.wav", "01234567-89ab-4def-8123-456789abcdef",
        "uneasy chamber strings, restrained and instrumental", 123456789};

    const auto json = creatures::dialogScriptToJson(script);
    EXPECT_EQ(json["background_music"]["sound_file"], script.background_music->sound_file);
    EXPECT_EQ(json["background_music"]["generation_id"], script.background_music->generation_id);
    EXPECT_EQ(json["background_music"]["prompt"], script.background_music->prompt);
    EXPECT_EQ(json["background_music"]["accepted_at"], script.background_music->accepted_at);

    const auto dto = creatures::convertToDto(script);
    const auto roundTrip = creatures::convertFromDto(dto.getPtr());
    ASSERT_TRUE(roundTrip.background_music.has_value());
    EXPECT_EQ(*roundTrip.background_music, *script.background_music);
}

TEST(DialogScript, ClearingBackgroundMusicOmitsReferenceFromJson) {
    creatures::DialogScript script;
    script.id = "7aaf11a0-d75d-4ca2-891d-9f3493dc66e4";
    script.title = "Moonlit argument";
    script.turns = {{"55a2af23-e797-462b-8c91-e0bc23b86fd4", "I knew you'd come back."}};
    script.background_music = creatures::DialogBackgroundMusic{
        "dialog/music/moonlit-argument--bgm--uneasy-strings--0123456789ab.wav", "01234567-89ab-4def-8123-456789abcdef",
        "uneasy chamber strings, restrained and instrumental", 123456789};

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
// Every field on this model needs to survive BOTH round trips, so these tests
// walk each one rather than only the JSON path that happened to be written.
// ===========================================================================

TEST(DialogScriptStageBindingTest, StageIdSurvivesConvertToDto) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "bound scene";
    script.stage_id = "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329";

    const auto dto = creatures::convertToDto(script);
    ASSERT_TRUE(dto->stage_id);
    EXPECT_EQ(std::string(*dto->stage_id), "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329");
}

TEST(DialogScriptStageBindingTest, UnboundScriptOmitsStageIdRatherThanSendingEmpty) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "unbound scene";

    const auto dto = creatures::convertToDto(script);
    EXPECT_FALSE(dto->stage_id) << "an unbound script should omit stage_id, not send an empty string";
}

TEST(DialogScriptStageBindingTest, StageIdSurvivesConvertFromDto) {
    auto dto = creatures::DialogScriptDto::createShared();
    dto->id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    dto->title = "bound scene";
    dto->stage_id = "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329";

    const auto script = creatures::convertFromDto(dto.getPtr());
    EXPECT_EQ(script.stage_id, "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329");
}

TEST(DialogScriptStageBindingTest, StageIdSurvivesTheFullDtoRoundTrip) {
    creatures::DialogScript original;
    original.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    original.title = "bound scene";
    original.stage_id = "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329";

    const auto dto = creatures::convertToDto(original);
    const auto back = creatures::convertFromDto(dto.getPtr());
    EXPECT_EQ(back.stage_id, original.stage_id);
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
// Both round trips, deliberately. The DTO conversion has been the gap twice
// now — stage_id (#123) and source_render_choices — both times because only
// the JSON path was covered and every HTTP response goes through the DTO.
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

TEST(AcceptedVoiceTest, SurvivesConvertToDto) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "scene";
    script.accepted_voice = sampleAcceptedVoice();

    const auto dto = creatures::convertToDto(script);
    ASSERT_TRUE(dto->accepted_voice);
    EXPECT_EQ(std::string(*dto->accepted_voice->generation_id), sampleAcceptedVoice().generation_id);
    EXPECT_EQ(std::string(*dto->accepted_voice->dialog_cache_key), sampleAcceptedVoice().dialog_cache_key);
    EXPECT_EQ(std::string(*dto->accepted_voice->sound_file), sampleAcceptedVoice().sound_file);
    EXPECT_EQ(*dto->accepted_voice->accepted_at, 1786000000000);
}

TEST(AcceptedVoiceTest, SurvivesTheFullDtoRoundTrip) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "scene";
    script.accepted_voice = sampleAcceptedVoice();

    const auto back = creatures::convertFromDto(creatures::convertToDto(script).getPtr());
    ASSERT_TRUE(back.accepted_voice.has_value());
    EXPECT_EQ(*back.accepted_voice, sampleAcceptedVoice());
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

    EXPECT_FALSE(creatures::convertToDto(script)->accepted_voice);
    EXPECT_FALSE(creatures::dialogScriptToJson(script).contains("accepted_voice"));
}

// ===========================================================================
// Music composition source (issue #136)
//
// Music is fitted to one voice take's timing and length. Without recording
// which take, the Console can flag stale *candidates* but the already-accepted
// card stays green — describing audio that will never render again. These pin
// all three conversion directions, because that asymmetry is exactly how
// stage_id (#123) and source_render_choices shipped broken: JSON covered, DTO
// silently dropping the field on every response.
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

TEST(MusicCompositionSource, SurvivesConvertToDto) {
    const auto script = scriptWithMusic();
    const auto dto = creatures::convertToDto(script);
    ASSERT_TRUE(dto->background_music);
    ASSERT_TRUE(dto->background_music->source_dialog_generation_id);
    EXPECT_EQ(std::string(*dto->background_music->source_dialog_generation_id),
              script.background_music->source_dialog_generation_id);
    ASSERT_TRUE(dto->background_music->source_dialog_cache_key);
    EXPECT_EQ(std::string(*dto->background_music->source_dialog_cache_key),
              script.background_music->source_dialog_cache_key);
}

TEST(MusicCompositionSource, SurvivesTheFullDtoRoundTrip) {
    const auto script = scriptWithMusic();
    const auto roundTrip = creatures::convertFromDto(creatures::convertToDto(script).getPtr());
    ASSERT_TRUE(roundTrip.background_music.has_value());
    EXPECT_EQ(*roundTrip.background_music, *script.background_music);
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

    const auto dto = creatures::convertToDto(script);
    ASSERT_TRUE(dto->background_music);
    EXPECT_FALSE(dto->background_music->source_dialog_generation_id);
    EXPECT_FALSE(dto->background_music->source_dialog_cache_key);

    // And the older shape still round-trips, so pre-#136 music keeps working.
    const auto roundTrip = creatures::convertFromDto(dto.getPtr());
    ASSERT_TRUE(roundTrip.background_music.has_value());
    EXPECT_EQ(*roundTrip.background_music, *script.background_music);
}
