#include <gtest/gtest.h>

#include "api/DialogContracts.h"

namespace creatures::api {
namespace {

constexpr const char *CREATURE_ID = "00000000-0000-4000-8000-000000000001";
constexpr const char *SCRIPT_ID = "00000000-0000-4000-8000-000000000002";
constexpr const char *GENERATION_ID = "00000000-0000-4000-8000-000000000003";

TEST(DialogContracts, ParsesLegacyNullOptionalsInQueuedDialogRequest) {
    const nlohmann::json json = {{"turns", nullptr},        {"script_id", SCRIPT_ID}, {"persistence", "permanent"},
                                 {"autoplay", nullptr},     {"title", nullptr},       {"stage_id", nullptr},
                                 {"generation_id", nullptr}};

    const auto result = dialogRequestFromJson(json);

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    EXPECT_EQ(result.getValue()->scriptId, SCRIPT_ID);
    EXPECT_FALSE(result.getValue()->autoplay);
    EXPECT_FALSE(result.getValue()->title.has_value());
}

TEST(DialogContracts, RejectsUnknownAndMalformedTurnFields) {
    const nlohmann::json json = {
        {"turns", {{{"creature_id", CREATURE_ID}, {"text", "hello"}, {"unexpected", true}}}},
        {"script_id", nullptr},
        {"persistence", "adhoc"},
    };

    const auto result = dialogRequestFromJson(json);

    ASSERT_FALSE(result.isSuccess());
    EXPECT_NE(result.getError()->getMessage().find("unknown field"), std::string::npos);
}

TEST(DialogContracts, RejectsMalformedDialogRequestIdentifiers) {
    nlohmann::json json = {{"turns", {{{"creature_id", CREATURE_ID}, {"text", "hello"}}}},
                           {"persistence", "adhoc"},
                           {"stage_id", "not-a-uuid"}};

    EXPECT_FALSE(dialogRequestFromJson(json).isSuccess());

    json["stage_id"] = SCRIPT_ID;
    json["generation_id"] = "not-a-uuid";
    EXPECT_FALSE(dialogRequestFromJson(json).isSuccess());
}

TEST(DialogContracts, ParsesAndBoundsDialogMusicRequest) {
    const nlohmann::json json = {{"script_id", SCRIPT_ID},
                                 {"dialog_cache_key", std::string(64, 'a')},
                                 {"dialog_generation_id", GENERATION_ID},
                                 {"prompt", "quiet instrumental underscore"},
                                 {"duration_extension_ms", 60001},
                                 {"generation_mode", "track"}};

    const auto result = dialogMusicRequestFromJson(json);

    ASSERT_FALSE(result.isSuccess());
    EXPECT_NE(result.getError()->getMessage().find("between 0 and 60000"), std::string::npos);
}

TEST(DialogContracts, ValidatesDialogMusicIdentifiersAndMode) {
    nlohmann::json json = {{"script_id", SCRIPT_ID},
                           {"dialog_cache_key", std::string(64, 'a')},
                           {"dialog_generation_id", GENERATION_ID},
                           {"prompt", "quiet instrumental underscore"},
                           {"generation_mode", "track"}};

    ASSERT_TRUE(dialogMusicRequestFromJson(json).isSuccess());

    json["dialog_cache_key"] = std::string(64, 'A');
    EXPECT_FALSE(dialogMusicRequestFromJson(json).isSuccess());
    json["dialog_cache_key"] = std::string(64, 'a');
    json["generation_mode"] = "surprise";
    EXPECT_FALSE(dialogMusicRequestFromJson(json).isSuccess());
}

// ---- #200: music generation controls -------------------------------------

nlohmann::json musicBase() {
    return {
        {"script_id", SCRIPT_ID}, {"dialog_cache_key", std::string(64, 'a')}, {"dialog_generation_id", GENERATION_ID}};
}

nlohmann::json generationChunk(int64_t durationMs) {
    return {{"text", "[Intro] warm strings"},
            {"duration_ms", durationMs},
            {"positive_styles", {"chamber orchestra", "playful"}},
            {"negative_styles", {"drums"}},
            {"context_adherence", "medium"}};
}

/// The exact body the console sends today must keep parsing unchanged; it
/// just lands on Music 2.5 with the documented defaults.
TEST(DialogContracts, Music200_LegacyPromptBodyStillParsesWithDefaults) {
    auto json = musicBase();
    json["prompt"] = "quiet instrumental underscore";
    json["duration_extension_ms"] = 3000;
    json["generation_mode"] = "loop";

    const auto result = dialogMusicRequestFromJson(json);

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    const auto request = result.getValue().value();
    EXPECT_FALSE(request.isPlanMode());
    EXPECT_EQ(request.prompt, "quiet instrumental underscore");
    EXPECT_EQ(request.durationExtensionMs, 3000);
    EXPECT_EQ(request.generationMode, "loop");
    EXPECT_EQ(request.modelId, "music_v2_5");
    EXPECT_TRUE(request.forceInstrumental);
    EXPECT_TRUE(request.storeForInpainting);
    EXPECT_FALSE(request.finetuneId.has_value());
    EXPECT_FALSE(request.finetuneStrength.has_value());
    EXPECT_FALSE(request.seed.has_value());
}

TEST(DialogContracts, Music200_PromptModeAcceptsNewKnobs) {
    auto json = musicBase();
    json["prompt"] = "a jingle the birds can sing";
    json["model_id"] = "music_v2";
    json["force_instrumental"] = false;
    json["finetune_id"] = "ft_abc";
    json["finetune_strength"] = 0.5;
    json["store_for_inpainting"] = false;

    const auto result = dialogMusicRequestFromJson(json);

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    const auto request = result.getValue().value();
    EXPECT_EQ(request.modelId, "music_v2");
    EXPECT_FALSE(request.forceInstrumental);
    EXPECT_EQ(request.finetuneId.value(), "ft_abc");
    EXPECT_DOUBLE_EQ(request.finetuneStrength.value(), 0.5);
    EXPECT_FALSE(request.storeForInpainting);
}

TEST(DialogContracts, Music200_RejectsBadKnobValues) {
    auto json = musicBase();
    json["prompt"] = "underscore";
    json["model_id"] = "music_v1";
    EXPECT_NE(dialogMusicRequestFromJson(json).getError()->getMessage().find("model_id"), std::string::npos);

    json = musicBase();
    json["prompt"] = "underscore";
    json["finetune_strength"] = 1.0;
    EXPECT_NE(dialogMusicRequestFromJson(json).getError()->getMessage().find("requires finetune_id"),
              std::string::npos);

    json = musicBase();
    json["prompt"] = "underscore";
    json["finetune_id"] = "ft";
    json["finetune_strength"] = 2.5;
    EXPECT_NE(dialogMusicRequestFromJson(json).getError()->getMessage().find("finetune_strength"), std::string::npos);

    json = musicBase();
    json["prompt"] = "underscore";
    json["seed"] = 7;
    EXPECT_NE(dialogMusicRequestFromJson(json).getError()->getMessage().find("seed only applies"), std::string::npos);

    json = musicBase();
    json["prompt"] = "underscore";
    json["mystery"] = 1;
    EXPECT_FALSE(dialogMusicRequestFromJson(json).isSuccess());

    json = musicBase();
    EXPECT_NE(dialogMusicRequestFromJson(json).getError()->getMessage().find("either prompt or composition_plan"),
              std::string::npos);
}

TEST(DialogContracts, Music200_ParsesCompositionPlanWithBothChunkKinds) {
    auto json = musicBase();
    json["seed"] = 12345;
    json["composition_plan"] = {
        {"chunks",
         {{{"song_id", "song-prior"}, {"range", {{"start_ms", 0}, {"end_ms", 12000}}}},
          {{"text", "[Bridge] {swelling strings}"},
           {"duration_ms", 18000},
           {"positive_styles", {"chamber orchestra"}},
           {"conditioning_ref", {{"song_id", "song-prior"}, {"range", {{"start_ms", 0}, {"end_ms", 12000}}}}},
           {"condition_strength", "xhigh"}}}}};

    const auto result = dialogMusicRequestFromJson(json);

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    const auto request = result.getValue().value();
    ASSERT_TRUE(request.isPlanMode());
    EXPECT_TRUE(request.prompt.empty());
    EXPECT_EQ(request.seed.value(), 12345);
    const auto &plan = request.compositionPlan.value();
    ASSERT_EQ(plan.chunks.size(), 2u);
    EXPECT_TRUE(plan.chunks[0].isAudioRef());
    EXPECT_EQ(plan.chunks[0].audioRef->songId, "song-prior");
    EXPECT_EQ(plan.chunks[0].effectiveDurationMs(), 12000);
    EXPECT_FALSE(plan.chunks[1].isAudioRef());
    EXPECT_EQ(plan.chunks[1].durationMs, 18000);
    EXPECT_EQ(plan.chunks[1].contextAdherence, "high"); // default
    EXPECT_TRUE(plan.chunks[1].negativeStyles.empty()); // optional
    ASSERT_TRUE(plan.chunks[1].conditioningRef.has_value());
    EXPECT_EQ(plan.chunks[1].conditionStrength.value(), "xhigh");
    EXPECT_EQ(plan.totalDurationMs(), 30000);
}

TEST(DialogContracts, Music200_RejectsCrossModeFieldsAndBadChunks) {
    const auto planOf = [](nlohmann::json chunks) {
        auto json = musicBase();
        json["composition_plan"] = {{"chunks", std::move(chunks)}};
        return json;
    };
    const auto messageOf = [](const nlohmann::json &json) {
        const auto result = dialogMusicRequestFromJson(json);
        return result.isSuccess() ? std::string("<accepted>") : result.getError()->getMessage();
    };

    auto json = planOf({generationChunk(5000)});
    json["prompt"] = "also a prompt";
    EXPECT_NE(messageOf(json).find("prompt cannot be combined"), std::string::npos);

    json = planOf({generationChunk(5000)});
    json["duration_extension_ms"] = 1000;
    EXPECT_NE(messageOf(json).find("duration_extension_ms only applies"), std::string::npos);

    json = planOf({generationChunk(5000)});
    json["generation_mode"] = "loop";
    EXPECT_NE(messageOf(json).find("generation_mode only applies"), std::string::npos);

    json = planOf({generationChunk(5000)});
    json["force_instrumental"] = true;
    EXPECT_NE(messageOf(json).find("force_instrumental only applies"), std::string::npos);

    EXPECT_NE(messageOf(planOf(nlohmann::json::array())).find("chunks must not be empty"), std::string::npos);

    // Chunk limits carry the full path so the console can point at the field.
    EXPECT_NE(messageOf(planOf({generationChunk(2999)})).find("composition_plan.chunks[0].duration_ms"),
              std::string::npos);
    EXPECT_NE(messageOf(planOf({generationChunk(120001)})).find("chunks[0].duration_ms"), std::string::npos);

    auto chunk = generationChunk(5000);
    chunk["context_adherence"] = "extreme";
    EXPECT_NE(messageOf(planOf({chunk})).find("context_adherence must be"), std::string::npos);

    chunk = generationChunk(5000);
    chunk["condition_strength"] = "high";
    EXPECT_NE(messageOf(planOf({chunk})).find("condition_strength requires conditioning_ref"), std::string::npos);

    chunk = generationChunk(5000);
    chunk["song_id"] = "both-kinds";
    EXPECT_NE(messageOf(planOf({chunk})).find("either an audio reference"), std::string::npos);

    chunk = generationChunk(5000);
    chunk.erase("positive_styles");
    EXPECT_NE(messageOf(planOf({chunk})).find("positive_styles is required"), std::string::npos);

    chunk = generationChunk(5000);
    chunk["positive_styles"] = nlohmann::json::array();
    for (int i = 0; i < 51; ++i)
        chunk["positive_styles"].push_back("style");
    EXPECT_NE(messageOf(planOf({chunk})).find("maximum is 50"), std::string::npos);

    nlohmann::json audioRef = {{"song_id", "s"}, {"range", {{"start_ms", 5000}, {"end_ms", 5000}}}};
    EXPECT_NE(messageOf(planOf({audioRef})).find("end_ms must be greater"), std::string::npos);
    audioRef["range"]["end_ms"] = 6000;
    EXPECT_NE(messageOf(planOf({audioRef})).find("range must span"), std::string::npos);

    nlohmann::json tooMany = nlohmann::json::array();
    for (int i = 0; i < 31; ++i)
        tooMany.push_back(generationChunk(5000));
    EXPECT_NE(messageOf(planOf(tooMany)).find("maximum is 30"), std::string::npos);

    nlohmann::json tooLong = nlohmann::json::array();
    for (int i = 0; i < 6; ++i)
        tooLong.push_back(generationChunk(120000));
    EXPECT_NE(messageOf(planOf(tooLong)).find("must be 3000-600000 ms"), std::string::npos);

    // Every per-field limit respected, but the whole plan would not fit in the
    // take's provenance (stored three times inside a 1 MiB iXML chunk).
    nlohmann::json tooBig = nlohmann::json::array();
    for (int i = 0; i < 30; ++i) {
        auto fat = generationChunk(5000);
        fat["text"] = std::string(6000, 'x');
        fat["positive_styles"] = nlohmann::json::array();
        for (int j = 0; j < 50; ++j)
            fat["positive_styles"].push_back(std::string(200, 'y'));
        tooBig.push_back(fat);
    }
    EXPECT_NE(messageOf(planOf(tooBig)).find("serializes to"), std::string::npos);
    EXPECT_NE(messageOf(planOf(tooBig)).find("maximum is 131072"), std::string::npos);
}

/// The request is stored as job details and re-parsed by the worker, so the
/// serializer must produce something the parser accepts and that compares
/// equal — in both modes.
TEST(DialogContracts, Music200_RequestJsonRoundTripsExactlyInBothModes) {
    auto promptJson = musicBase();
    promptJson["prompt"] = "underscore";
    promptJson["duration_extension_ms"] = 2500;
    promptJson["generation_mode"] = "ambience";
    promptJson["force_instrumental"] = false;
    promptJson["finetune_id"] = "ft_abc";
    promptJson["finetune_strength"] = 1.25;
    promptJson["store_for_inpainting"] = false;
    const auto promptRequest = dialogMusicRequestFromJson(promptJson).getValue().value();
    const auto promptAgain = dialogMusicRequestFromJson(dialogMusicRequestToJson(promptRequest));
    ASSERT_TRUE(promptAgain.isSuccess()) << promptAgain.getError()->getMessage();
    EXPECT_EQ(dialogMusicRequestToJson(promptAgain.getValue().value()), dialogMusicRequestToJson(promptRequest));
    EXPECT_EQ(promptAgain.getValue()->finetuneStrength.value(), 1.25);

    auto planJson = musicBase();
    planJson["seed"] = 99;
    planJson["model_id"] = "music_v2";
    planJson["composition_plan"] = {
        {"chunks",
         {{{"song_id", "song-prior"}, {"range", {{"start_ms", 0}, {"end_ms", 12000}}}}, generationChunk(18000)}}};
    const auto planRequest = dialogMusicRequestFromJson(planJson).getValue().value();
    const auto serialized = dialogMusicRequestToJson(planRequest);
    EXPECT_FALSE(serialized.contains("prompt"));
    EXPECT_FALSE(serialized.contains("generation_mode"));
    EXPECT_FALSE(serialized.contains("force_instrumental"));
    EXPECT_EQ(serialized["seed"], 99);
    const auto planAgain = dialogMusicRequestFromJson(serialized);
    ASSERT_TRUE(planAgain.isSuccess()) << planAgain.getError()->getMessage();
    EXPECT_EQ(planAgain.getValue()->compositionPlan.value(), planRequest.compositionPlan.value());
    EXPECT_EQ(dialogMusicRequestToJson(planAgain.getValue().value()), serialized);
}

TEST(DialogContracts, Music200_GenerationResultKeepsLegacyKeysAndAddsRecipe) {
    DialogMusicGenerationResult result;
    result.musicGenerationId = GENERATION_ID;
    result.mp3Url = "/api/v1/animation/dialog/music/generated/x.mp3";
    result.durationSeconds = 12.5;
    result.dialogDurationMs = 10000;
    result.durationExtensionMs = 2500;
    result.requestedMusicLengthMs = 12500;
    result.prompt = "underscore";
    result.recipe.modelId = "music_v2_5";
    result.recipe.songId = "song-1";
    result.recipe.requestKind = "prompt";
    result.recipe.prompt = "underscore";
    result.recipe.generationMode = "track";
    result.recipe.forceInstrumental = true;
    result.recipe.storedForInpainting = true;
    result.recipe.compositionPlan = {{"chunks", nlohmann::json::array()}};

    const auto json = dialogMusicGenerationResultToJson(result);

    // Pre-#200 keys, exactly as before.
    EXPECT_EQ(json["music_generation_id"], GENERATION_ID);
    EXPECT_EQ(json["mp3_url"], "/api/v1/animation/dialog/music/generated/x.mp3");
    EXPECT_EQ(json["duration_seconds"], 12.5);
    EXPECT_EQ(json["dialog_duration_ms"], 10000);
    EXPECT_EQ(json["duration_extension_ms"], 2500);
    EXPECT_EQ(json["requested_music_length_ms"], 12500);
    EXPECT_EQ(json["prompt"], "underscore");
    // Recipe keys beside them.
    EXPECT_EQ(json["model_id"], "music_v2_5");
    EXPECT_EQ(json["song_id"], "song-1");
    EXPECT_EQ(json["request_kind"], "prompt");
    EXPECT_EQ(json["generation_mode"], "track");
    EXPECT_EQ(json["force_instrumental"], true);
    EXPECT_EQ(json["stored_for_inpainting"], true);
    EXPECT_TRUE(json["composition_plan"].is_object());
    EXPECT_FALSE(json.contains("seed"));
    EXPECT_FALSE(json.contains("finetune_id"));
}

/// A plan exactly as `POST /v1/music/plan` returned it on 2026-09-18 (Music
/// 2.5) must be accepted unchanged: the console drafts, maybe edits, and
/// submits. Note the explicit nulls ElevenLabs emits for conditioning.
TEST(DialogContracts, Music200_AcceptsElevenLabsPlanOutputVerbatim) {
    const auto elevenLabsPlan = nlohmann::json::parse(R"({"chunks":[
        {"text":"[Tease]","duration_ms":6000,
         "positive_styles":["74 BPM","playful pizzicato strings","instrumental underscore"],
         "negative_styles":["drums","percussion"],
         "context_adherence":"high","conditioning_ref":null,"condition_strength":null},
        {"text":"[Reveal]","duration_ms":9000,
         "positive_styles":["bolder strings"],"negative_styles":[],
         "context_adherence":"high","conditioning_ref":null,"condition_strength":null}]})");
    auto json = musicBase();
    json["composition_plan"] = elevenLabsPlan;

    const auto result = dialogMusicRequestFromJson(json);

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    const auto plan = result.getValue()->compositionPlan.value();
    ASSERT_EQ(plan.chunks.size(), 2u);
    EXPECT_FALSE(plan.chunks[0].conditioningRef.has_value());
    EXPECT_FALSE(plan.chunks[0].conditionStrength.has_value());
    EXPECT_EQ(plan.totalDurationMs(), 15000);
    // And what we send upstream omits the nulls rather than echoing them.
    const auto upstream = creatures::voice::musicPlanChunkToJson(plan.chunks[0]);
    EXPECT_FALSE(upstream.contains("conditioning_ref"));
    EXPECT_FALSE(upstream.contains("condition_strength"));
}

TEST(DialogContracts, Music200_ParsesPlanRequest) {
    nlohmann::json json = {{"dialog_cache_key", std::string(64, 'a')},
                           {"dialog_generation_id", GENERATION_ID},
                           {"prompt", "playful underscore"},
                           {"duration_extension_ms", 1000}};
    auto result = dialogMusicPlanRequestFromJson(json);
    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    EXPECT_EQ(result.getValue()->modelId, "music_v2_5");
    EXPECT_FALSE(result.getValue()->sourceCompositionPlan.has_value());

    json["source_composition_plan"] = {{"chunks", {generationChunk(5000)}}};
    json["model_id"] = "music_v2";
    result = dialogMusicPlanRequestFromJson(json);
    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    EXPECT_EQ(result.getValue()->sourceCompositionPlan->chunks.size(), 1u);

    json["model_id"] = "music_v1";
    EXPECT_FALSE(dialogMusicPlanRequestFromJson(json).isSuccess());
    json.erase("model_id");
    json.erase("prompt");
    EXPECT_FALSE(dialogMusicPlanRequestFromJson(json).isSuccess());
}

TEST(DialogContracts, ParsesStrictAcceptVoiceTakeRequest) {
    const nlohmann::json json = {
        {"script_id", SCRIPT_ID}, {"generation_id", GENERATION_ID}, {"dialog_cache_key", std::string(64, 'a')}};

    const auto result = acceptVoiceTakeRequestFromJson(json);

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    EXPECT_EQ(result.getValue()->scriptId, SCRIPT_ID);
    EXPECT_EQ(result.getValue()->generationId, GENERATION_ID);
    EXPECT_EQ(result.getValue()->dialogCacheKey, std::string(64, 'a'));

    auto unknown = json;
    unknown["unexpected"] = true;
    EXPECT_FALSE(acceptVoiceTakeRequestFromJson(unknown).isSuccess());
}

TEST(DialogContracts, PreviewLookupUsesTheSameStrictTurnContract) {
    const nlohmann::json json = {
        {"turns", {{{"creature_id", CREATURE_ID}, {"text", "hello"}}}},
    };

    const auto result = dialogPreviewLookupRequestFromJson(json);

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    ASSERT_EQ(result.getValue()->size(), 1);
    EXPECT_EQ(result.getValue()->front().creatureId, CREATURE_ID);

    auto unknown = json;
    unknown["regenerate"] = true;
    EXPECT_FALSE(dialogPreviewLookupRequestFromJson(unknown).isSuccess());
}

TEST(DialogContracts, SerializesCanonicalQueuedRequests) {
    DialogRequest dialog{{{CREATURE_ID, "hello"}}, std::nullopt, "adhoc", true, "A scene", std::nullopt, GENERATION_ID};
    const auto dialogJson = dialogRequestToJson(dialog);
    EXPECT_EQ(dialogJson.at("turns").at(0).at("creature_id"), CREATURE_ID);
    EXPECT_EQ(dialogJson.at("generation_id"), GENERATION_ID);
    EXPECT_FALSE(dialogJson.contains("script_id"));

    DialogPreviewRequest preview{{{CREATURE_ID, "hello"}}, GENERATION_ID, false, std::nullopt};
    const auto previewJson = dialogPreviewRequestToJson(preview);
    EXPECT_EQ(previewJson.at("generation_id"), GENERATION_ID);
    EXPECT_FALSE(previewJson.contains("title"));
}

TEST(DialogContracts, SerializesPreviewMetadataWithStableWireKeys) {
    DialogPreviewMetaResponse response;
    response.cacheKey = std::string(64, 'b');
    response.generationId = GENERATION_ID;
    response.cached = true;
    response.audioUrl = "/audio.wav";
    response.audioFormat = "pcm_48000";
    response.sampleRate = 48000;
    response.durationSeconds = 1.25;
    response.voiceSegments.push_back({"voice-1", 1, 2, 3});
    response.forcedAlignmentWords.push_back({"hello", 0.0, 0.5});
    response.forcedAlignmentLoss = 0.02;

    const auto json = dialogPreviewMetaResponseToJson(response);

    EXPECT_EQ(json.at("generation_id"), GENERATION_ID);
    EXPECT_EQ(json.at("voice_segments").at(0).at("dialog_input_index"), 3);
    EXPECT_EQ(json.at("forced_alignment_words").at(0).at("text"), "hello");
    EXPECT_TRUE(json.at("forced_alignment_chars").empty());
}

TEST(DialogContracts, RejectsUnsafePreviewGenerationIdAndCreatureId) {
    DialogPreviewRequest request{{{CREATURE_ID, "hello"}}, std::string("../escaped"), false, std::nullopt};
    EXPECT_FALSE(validateDialogPreviewRequest(request).isSuccess());

    request.generationId = GENERATION_ID;
    request.turns.front().creatureId = "not-a-uuid";
    EXPECT_FALSE(validateDialogPreviewRequest(request).isSuccess());
}

TEST(DialogContracts, SerializesLookupAndValidationResponses) {
    DialogPreviewLookupResponse lookup{std::string(64, 'c'), {{GENERATION_ID, "2026-08-30T12:00:00Z"}}, GENERATION_ID};
    const auto lookupJson = dialogPreviewLookupResponseToJson(lookup);
    EXPECT_EQ(lookupJson.at("generations").at(0).at("generation_id"), GENERATION_ID);
    EXPECT_EQ(lookupJson.at("latest_generation_id"), GENERATION_ID);

    // #204: nothing cached is still a 200 with the deterministic key — the
    // console needs it to judge acceptance freshness — and no latest id.
    const DialogPreviewLookupResponse empty{std::string(64, 'c'), {}, {}};
    const auto emptyJson = dialogPreviewLookupResponseToJson(empty);
    EXPECT_EQ(emptyJson.at("cache_key"), std::string(64, 'c'));
    EXPECT_TRUE(emptyJson.at("generations").is_array());
    EXPECT_TRUE(emptyJson.at("generations").empty());
    EXPECT_FALSE(emptyJson.contains("latest_generation_id"));

    DialogScriptValidationResponse validation;
    validation.valid = false;
    validation.turnCount = 2;
    validation.errorMessages = {"bad turn"};
    const auto validationJson = dialogScriptValidationResponseToJson(validation);
    EXPECT_FALSE(validationJson.at("valid"));
    EXPECT_FALSE(validationJson.contains("script_id"));
    EXPECT_EQ(validationJson.at("error_messages").at(0), "bad turn");
}

} // namespace
} // namespace creatures::api
