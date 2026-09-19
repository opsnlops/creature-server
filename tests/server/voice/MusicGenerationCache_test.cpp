#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/storage/Storage.h"
#include "server/voice/MusicGenerationCache.h"

namespace {

using creatures::voice::CachedMusicGeneration;
using creatures::voice::MusicWavProvenance;

/// Writes into the real GenerationCache root (under the system temp dir, no
/// Configuration needed) and removes its own files afterwards.
class MusicGenerationCacheTest : public ::testing::Test {
  protected:
    static CachedMusicGeneration candidate(const std::string &generationId, const std::string &requestKind,
                                           const std::string &prompt) {
        CachedMusicGeneration generation;
        generation.generationId = generationId;
        generation.scriptId = "00000000-0000-4000-8000-000000000002";
        generation.title = "Cache Test";
        generation.prompt = prompt;
        generation.generationMode = requestKind == "prompt" ? "track" : "";
        generation.durationSeconds = 0.01;
        generation.provenance.fileUid = generationId;
        generation.provenance.take = generationId;
        MusicWavProvenance music;
        music.provider = "ElevenLabs";
        music.modelId = "music_v2_5";
        music.requestKind = requestKind;
        music.prompt = prompt;
        music.requestJson = requestKind == "prompt" ? R"({"prompt":"x"})" : R"({"composition_plan":{"chunks":[]}})";
        music.compositionPlanJson = R"({"chunks":[]})";
        music.musicGenerationId = generationId;
        music.songId = "song-1";
        music.seed = requestKind == "prompt" ? std::nullopt : std::optional<int64_t>{4242};
        generation.provenance.music = music;
        return generation;
    }

    void TearDown() override {
        auto root = creatures::storage::root(creatures::storage::Persistence::GenerationCache);
        if (!root.isSuccess())
            return;
        for (const auto &id : created_) {
            for (const char *extension : {".wav", ".json", ".mp3"}) {
                std::error_code ignored;
                std::filesystem::remove(root.getValue().value() / "music" / (id + extension), ignored);
            }
        }
    }

    std::vector<std::string> created_;
    const std::vector<uint8_t> pcm_ = std::vector<uint8_t>(960, 0);
};

/// #200 regression: a composition-plan take has no prompt. The loader used
/// to require one, so plan-mode takes could be generated but never
/// auditioned, inspected, or promoted (found on prod, 3.47.0).
TEST_F(MusicGenerationCacheTest, PlanModeTakeWithoutPromptRoundTrips) {
    const std::string id = "6f0f6c4e-9a3b-4c6d-8e1f-2a3b4c5d6e70";
    created_.push_back(id);
    auto saved = creatures::voice::saveMusicGeneration(candidate(id, "composition_plan", ""), pcm_);
    ASSERT_TRUE(saved.isSuccess()) << saved.getError()->getMessage();

    auto loaded = creatures::voice::loadMusicGeneration(id);
    ASSERT_TRUE(loaded.isSuccess()) << loaded.getError()->getMessage();
    const auto generation = loaded.getValue().value();
    EXPECT_TRUE(generation.prompt.empty());
    ASSERT_TRUE(generation.provenance.music.has_value());
    EXPECT_EQ(generation.provenance.music->requestKind, "composition_plan");
    EXPECT_EQ(generation.provenance.music->seed.value(), 4242);
    EXPECT_EQ(generation.provenance.music->songId, "song-1");
}

/// #202: a library candidate is composed against no dialog, so it has no
/// script id. The loader must accept empty (and still reject garbage).
TEST_F(MusicGenerationCacheTest, DialogFreeCandidateHasNoScriptId) {
    const std::string id = "6f0f6c4e-9a3b-4c6d-8e1f-2a3b4c5d6e73";
    created_.push_back(id);
    auto generation = candidate(id, "composition_plan", "");
    generation.scriptId.clear();
    generation.provenance.music->pieceId = "piece-1";
    auto saved = creatures::voice::saveMusicGeneration(generation, pcm_);
    ASSERT_TRUE(saved.isSuccess()) << saved.getError()->getMessage();
    auto loaded = creatures::voice::loadMusicGeneration(id);
    ASSERT_TRUE(loaded.isSuccess()) << loaded.getError()->getMessage();
    EXPECT_TRUE(loaded.getValue()->scriptId.empty());
    EXPECT_EQ(loaded.getValue()->provenance.music->pieceId, "piece-1");

    generation.scriptId = "not-a-uuid";
    EXPECT_FALSE(creatures::voice::saveMusicGeneration(generation, pcm_).isSuccess());
}

/// #202 regression: a sections-mode take has no prompt either. The 3.47.0
/// fix only exempted composition_plan, and prod rejected every library
/// candidate (found on prod, 3.48.0).
TEST_F(MusicGenerationCacheTest, SectionsModeTakeWithoutPromptRoundTrips) {
    const std::string id = "6f0f6c4e-9a3b-4c6d-8e1f-2a3b4c5d6e74";
    created_.push_back(id);
    auto generation = candidate(id, "sections", "");
    generation.scriptId.clear();
    generation.provenance.music->sectionsJson = R"([{"text":"[Intro]","duration_ms":8000,"positive_styles":["a"]}])";
    ASSERT_TRUE(creatures::voice::saveMusicGeneration(generation, pcm_).isSuccess());
    auto loaded = creatures::voice::loadMusicGeneration(id);
    ASSERT_TRUE(loaded.isSuccess()) << loaded.getError()->getMessage();
    EXPECT_EQ(loaded.getValue()->provenance.music->requestKind, "sections");
}

TEST_F(MusicGenerationCacheTest, PromptModeTakeStillRequiresItsPrompt) {
    const std::string id = "6f0f6c4e-9a3b-4c6d-8e1f-2a3b4c5d6e71";
    created_.push_back(id);
    auto saved = creatures::voice::saveMusicGeneration(candidate(id, "prompt", ""), pcm_);
    ASSERT_TRUE(saved.isSuccess()) << saved.getError()->getMessage();

    auto loaded = creatures::voice::loadMusicGeneration(id);
    EXPECT_FALSE(loaded.isSuccess());

    const std::string good = "6f0f6c4e-9a3b-4c6d-8e1f-2a3b4c5d6e72";
    created_.push_back(good);
    ASSERT_TRUE(creatures::voice::saveMusicGeneration(candidate(good, "prompt", "quiet strings"), pcm_).isSuccess());
    auto loadedGood = creatures::voice::loadMusicGeneration(good);
    ASSERT_TRUE(loadedGood.isSuccess()) << loadedGood.getError()->getMessage();
    EXPECT_EQ(loadedGood.getValue()->prompt, "quiet strings");
}

} // namespace
