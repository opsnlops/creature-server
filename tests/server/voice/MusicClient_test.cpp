#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/ElevenLabsHttp.h"
#include "server/voice/MusicClient.h"

TEST(ElevenLabsHttp, CapturesMusicResponseIdentifiersCaseInsensitively) {
    creatures::voice::elevenlabs_http::ResponseHeaders headers;
    std::string requestId = "X-ElevenLabs-Request-ID: request-123\r\n";
    std::string songId = "Song-ID: song-456\r\n";
    EXPECT_EQ(creatures::voice::elevenlabs_http::captureResponseHeader(requestId.data(), 1, requestId.size(), &headers),
              requestId.size());
    EXPECT_EQ(creatures::voice::elevenlabs_http::captureResponseHeader(songId.data(), 1, songId.size(), &headers),
              songId.size());
    EXPECT_EQ(headers.requestId, "request-123");
    EXPECT_EQ(headers.songId, "song-456");
}

TEST(MusicClient, ParsesDetailedMultipartPcmAndMetadata) {
    const std::string boundary = "music-boundary";
    const std::string json =
        R"({"composition_plan":{"chunks":[{"duration_ms":3000}]},"song_metadata":{"title":"BGM"}})";
    std::string body = "--" + boundary + "\r\nContent-Type: application/json\r\n\r\n" + json + "\r\n";
    body += "--" + boundary + "\r\nContent-Type: application/octet-stream\r\n\r\n";
    body.push_back('\x01');
    body.push_back('\x00');
    body.push_back('\x02');
    body.push_back('\x00');
    body += "\r\n--" + boundary + "--\r\n";

    const std::vector<uint8_t> bytes(body.begin(), body.end());
    auto result =
        creatures::voice::MusicClient::parseDetailedResponse(bytes, "multipart/mixed; boundary=\"music-boundary\"");
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage()
                                                          : std::string("unexpected parse failure"));
    const auto parsed = result.getValue().value();
    ASSERT_EQ(parsed.audioPcm.size(), 2u);
    EXPECT_EQ(parsed.audioPcm[0], 1);
    EXPECT_EQ(parsed.sourceChannels, 2);
    EXPECT_EQ(parsed.responseMetadata["song_metadata"]["title"], "BGM");
    EXPECT_EQ(parsed.compositionPlan["chunks"][0]["duration_ms"], 3000);
    EXPECT_EQ(parsed.songMetadata["title"], "BGM");
}

TEST(MusicClient, RejectsMultipartWithoutAudio) {
    const std::string body = "--x\r\nContent-Type: application/json\r\n\r\n{}\r\n--x--\r\n";
    auto result = creatures::voice::MusicClient::parseDetailedResponse(std::vector<uint8_t>(body.begin(), body.end()),
                                                                       "multipart/mixed; boundary=x");
    EXPECT_FALSE(result.isSuccess());
}

// ---- #200: request body builder + helper-endpoint parsers ----------------

TEST(MusicClient, BuildsPromptModeBodyWithoutPlanOnlyFields) {
    creatures::voice::MusicGenerationRequest request;
    request.prompt = "quiet underscore";
    request.musicLengthMs = 42000;
    request.generationMode = "loop";
    request.forceInstrumental = false;
    request.finetuneId = "ft_abc";
    request.finetuneStrength = 0.75;
    request.storeForInpainting = false;
    request.seed = 5; // ignored in prompt mode: ElevenLabs rejects seed with prompt

    const auto body = creatures::voice::musicGenerationRequestToElevenLabsJson(request);

    EXPECT_EQ(body["model_id"], "music_v2_5");
    EXPECT_EQ(body["prompt"], "quiet underscore");
    EXPECT_EQ(body["music_length_ms"], 42000);
    EXPECT_EQ(body["generation_mode"], "loop");
    EXPECT_EQ(body["force_instrumental"], false);
    EXPECT_EQ(body["finetune_id"], "ft_abc");
    EXPECT_DOUBLE_EQ(body["finetune_strength"].get<double>(), 0.75);
    EXPECT_EQ(body["store_for_inpainting"], false);
    EXPECT_EQ(body["with_timestamps"], false);
    EXPECT_EQ(body["sign_with_c2pa"], false);
    EXPECT_FALSE(body.contains("seed"));
    EXPECT_FALSE(body.contains("composition_plan"));
}

TEST(MusicClient, BuildsPlanModeBodyWithoutPromptOnlyFields) {
    creatures::voice::MusicGenerationRequest request;
    request.modelId = "music_v2";
    request.seed = 4242;
    creatures::voice::MusicCompositionPlan plan;
    creatures::voice::MusicPlanChunk ref;
    ref.audioRef = creatures::voice::MusicAudioRange{"song-prior", 0, 12000};
    creatures::voice::MusicPlanChunk gen;
    gen.text = "[Bridge] {swelling strings}";
    gen.durationMs = 18000;
    gen.positiveStyles = {"chamber orchestra"};
    gen.contextAdherence = "low";
    gen.conditioningRef = creatures::voice::MusicAudioRange{"song-prior", 0, 12000};
    gen.conditionStrength = "medium";
    plan.chunks = {ref, gen};
    request.compositionPlan = plan;

    const auto body = creatures::voice::musicGenerationRequestToElevenLabsJson(request);

    EXPECT_EQ(body["model_id"], "music_v2");
    EXPECT_EQ(body["seed"], 4242);
    EXPECT_EQ(body["store_for_inpainting"], true);
    EXPECT_FALSE(body.contains("prompt"));
    EXPECT_FALSE(body.contains("music_length_ms"));
    EXPECT_FALSE(body.contains("generation_mode"));
    EXPECT_FALSE(body.contains("force_instrumental"));
    EXPECT_FALSE(body.contains("finetune_id"));
    const auto &chunks = body["composition_plan"]["chunks"];
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0], (nlohmann::json{{"song_id", "song-prior"}, {"range", {{"start_ms", 0}, {"end_ms", 12000}}}}));
    EXPECT_EQ(chunks[1]["text"], "[Bridge] {swelling strings}");
    EXPECT_EQ(chunks[1]["duration_ms"], 18000);
    EXPECT_EQ(chunks[1]["positive_styles"], nlohmann::json({"chamber orchestra"}));
    EXPECT_EQ(chunks[1]["negative_styles"], nlohmann::json::array());
    EXPECT_EQ(chunks[1]["context_adherence"], "low");
    EXPECT_EQ(chunks[1]["conditioning_ref"]["song_id"], "song-prior");
    EXPECT_EQ(chunks[1]["condition_strength"], "medium");
    EXPECT_EQ(plan.totalDurationMs(), 30000);
}

TEST(MusicClient, ParsesPlanResponseAndRejectsV1Shape) {
    auto ok = creatures::voice::MusicClient::parsePlanResponse(
        R"({"chunks":[{"text":"[Intro]","duration_ms":5000,"positive_styles":["a"],"negative_styles":[]}]})");
    ASSERT_TRUE(ok.isSuccess()) << ok.getError()->getMessage();
    EXPECT_EQ(ok.getValue()->compositionPlan["chunks"].size(), 1u);

    // music_v1 plan shape (sections) means the model_id didn't take.
    auto v1 = creatures::voice::MusicClient::parsePlanResponse(
        R"({"positive_global_styles":[],"negative_global_styles":[],"sections":[]})");
    EXPECT_FALSE(v1.isSuccess());
    EXPECT_FALSE(creatures::voice::MusicClient::parsePlanResponse("not json").isSuccess());
}

TEST(MusicClient, ParsesFinetuneList) {
    auto result = creatures::voice::MusicClient::parseFinetunesResponse(R"({
        "finetunes": [
            {"id": "ft_1", "name": "Bird Bops", "tags": ["pop", "chirpy"], "primary_genre": "pop",
             "model_id": "music_v2_5", "created_at": "2026-09-01T00:00:00Z", "visibility": "private",
             "created_by": "self", "status": "completed", "training_progress": 1.0},
            {"id": "ft_2", "name": "Half", "tags": [], "primary_genre": null, "model_id": "music_v2",
             "status": "in_progress", "training_progress": 0.4},
            {"name": "no id — skipped"}
        ],
        "next_cursor": null, "has_more": false})");
    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    const auto finetunes = result.getValue().value();
    ASSERT_EQ(finetunes.size(), 2u);
    EXPECT_EQ(finetunes[0].id, "ft_1");
    EXPECT_EQ(finetunes[0].name, "Bird Bops");
    EXPECT_EQ(finetunes[0].tags, (std::vector<std::string>{"pop", "chirpy"}));
    EXPECT_EQ(finetunes[0].primaryGenre.value(), "pop");
    EXPECT_EQ(finetunes[0].modelId, "music_v2_5");
    EXPECT_EQ(finetunes[0].status, "completed");
    EXPECT_FALSE(finetunes[1].primaryGenre.has_value());
    EXPECT_DOUBLE_EQ(finetunes[1].trainingProgress, 0.4);
    EXPECT_FALSE(creatures::voice::MusicClient::parseFinetunesResponse(R"({"nope":[]})").isSuccess());
}
