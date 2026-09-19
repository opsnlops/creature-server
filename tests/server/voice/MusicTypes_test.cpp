#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "server/voice/MusicTypes.h"

using creatures::voice::buildMusicSectionsPlan;
using creatures::voice::MusicBaseVersion;
using creatures::voice::MusicCompositionPlan;
using creatures::voice::MusicSection;
using creatures::voice::MusicSectionsPlanError;

namespace {

MusicSection section(const std::string &text, int64_t durationMs) {
    MusicSection s;
    s.text = text;
    s.durationMs = durationMs;
    s.positiveStyles = {"pizzicato strings"};
    s.negativeStyles = {"drums"};
    return s;
}

MusicBaseVersion base() {
    return {"song-base", {section("[Intro]", 8000), section("[Verse]", 40000), section("[Outro]", 5000)}};
}

MusicCompositionPlan planOf(const std::variant<MusicCompositionPlan, MusicSectionsPlanError> &result) {
    EXPECT_TRUE(std::holds_alternative<MusicCompositionPlan>(result))
        << std::get<MusicSectionsPlanError>(result).message;
    return std::get<MusicCompositionPlan>(result);
}

std::string errorOf(const std::variant<MusicCompositionPlan, MusicSectionsPlanError> &result) {
    EXPECT_TRUE(std::holds_alternative<MusicSectionsPlanError>(result));
    return std::holds_alternative<MusicSectionsPlanError>(result) ? std::get<MusicSectionsPlanError>(result).message
                                                                  : "<built>";
}

} // namespace

/// Kept sections become audio-refs over the base's cumulative span; a changed
/// section is regenerated, conditioned on the base's span at that index.
TEST(MusicSectionsPlan, KeepsByAudioRefAndConditionsChangedSections) {
    auto sections = base().sections;
    sections[1].positiveStyles = {"pizzicato strings", "warm cello"};
    const auto plan = planOf(buildMusicSectionsPlan(sections, base(), {0, 2}, "medium"));

    ASSERT_EQ(plan.chunks.size(), 3u);
    ASSERT_TRUE(plan.chunks[0].isAudioRef());
    EXPECT_EQ(plan.chunks[0].audioRef->songId, "song-base");
    EXPECT_EQ(plan.chunks[0].audioRef->startMs, 0);
    EXPECT_EQ(plan.chunks[0].audioRef->endMs, 8000);
    EXPECT_FALSE(plan.chunks[1].isAudioRef());
    EXPECT_EQ(plan.chunks[1].positiveStyles, (std::vector<std::string>{"pizzicato strings", "warm cello"}));
    ASSERT_TRUE(plan.chunks[1].conditioningRef.has_value());
    EXPECT_EQ(plan.chunks[1].conditioningRef->startMs, 8000);
    EXPECT_EQ(plan.chunks[1].conditioningRef->endMs, 8000 + 30000); // 40 s section, capped at 30 s
    EXPECT_EQ(plan.chunks[1].conditionStrength.value(), "medium");
    ASSERT_TRUE(plan.chunks[2].isAudioRef());
    EXPECT_EQ(plan.chunks[2].audioRef->startMs, 48000);
    EXPECT_EQ(plan.chunks[2].audioRef->endMs, 53000);
    EXPECT_EQ(plan.totalDurationMs(), 53000);
}

TEST(MusicSectionsPlan, RejectsKeepOfAChangedSectionNamingTheField) {
    auto sections = base().sections;
    sections[0].durationMs = 9000;
    EXPECT_NE(errorOf(buildMusicSectionsPlan(sections, base(), {0}, "medium")).find("sections[0]"), std::string::npos);
    EXPECT_NE(errorOf(buildMusicSectionsPlan(sections, base(), {0}, "medium")).find("duration_ms"), std::string::npos);

    sections = base().sections;
    sections[2].text = "[Outro] {bigger}";
    EXPECT_NE(errorOf(buildMusicSectionsPlan(sections, base(), {2}, "medium")).find("text differs"), std::string::npos);
}

TEST(MusicSectionsPlan, RejectsKeepWithoutABaseOrBeyondIt) {
    const auto sections = base().sections;
    EXPECT_NE(errorOf(buildMusicSectionsPlan(sections, std::nullopt, {0}, "medium")).find("requires base_version_id"),
              std::string::npos);
    auto longer = sections;
    longer.push_back(section("[Coda]", 4000));
    EXPECT_NE(errorOf(buildMusicSectionsPlan(longer, base(), {3}, "medium")).find("has no section 3"),
              std::string::npos);
}

/// No base: a brand-new piece is plain generation chunks, no conditioning.
/// Added sections beyond the base are generated without conditioning too.
TEST(MusicSectionsPlan, NoBaseMeansPlainGeneration) {
    auto sections = base().sections;
    sections.push_back(section("[Coda]", 4000));
    const auto fresh = planOf(buildMusicSectionsPlan(sections, std::nullopt, {}, "medium"));
    ASSERT_EQ(fresh.chunks.size(), 4u);
    for (const auto &chunk : fresh.chunks) {
        EXPECT_FALSE(chunk.isAudioRef());
        EXPECT_FALSE(chunk.conditioningRef.has_value());
    }
    const auto extended = planOf(buildMusicSectionsPlan(sections, base(), {0, 1, 2}, "high"));
    ASSERT_EQ(extended.chunks.size(), 4u);
    EXPECT_TRUE(extended.chunks[2].isAudioRef());
    EXPECT_FALSE(extended.chunks[3].isAudioRef());
    EXPECT_FALSE(extended.chunks[3].conditioningRef.has_value());
}

/// A base section shorter than a legal chunk can't be a conditioning
/// reference; the section is simply regenerated unconditioned.
TEST(MusicSectionsPlan, SkipsConditioningOnTooShortBaseSpans) {
    MusicBaseVersion tiny{"song-tiny", {section("[Sting]", 3000)}};
    tiny.sections[0].durationMs = 2000; // not a legal chunk by itself (pre-validation base data)
    auto sections = std::vector<MusicSection>{section("[Sting] {new}", 5000)};
    const auto plan = planOf(buildMusicSectionsPlan(sections, tiny, {}, "medium"));
    ASSERT_EQ(plan.chunks.size(), 1u);
    EXPECT_FALSE(plan.chunks[0].conditioningRef.has_value());
}

TEST(MusicSectionsDiff, ReportsChangedAndKeptByIndex) {
    const auto original = base().sections;
    auto proposed = original;
    proposed[1].negativeStyles = {"drums", "vocals"};
    proposed.push_back(section("[Coda]", 4000));
    const auto diff = creatures::voice::diffMusicSections(original, proposed);
    EXPECT_EQ(diff.kept, (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(diff.changed, (std::vector<std::size_t>{1, 3}));

    // Shorter proposal: the dropped tail is simply not there to keep.
    const auto shorter = creatures::voice::diffMusicSections(original, {original[0]});
    EXPECT_EQ(shorter.kept, (std::vector<std::size_t>{0}));
    EXPECT_TRUE(shorter.changed.empty());
}

TEST(MusicSectionsDiff, NormalisesPlannerChunksToSections) {
    const auto chunk = nlohmann::json::parse(R"({"text":"[Tease]","duration_ms":6000,
        "positive_styles":["a"],"negative_styles":["b"],"context_adherence":"high",
        "conditioning_ref":null,"condition_strength":null})");
    const auto section = creatures::voice::planChunkToSectionJson(chunk);
    EXPECT_EQ(section, nlohmann::json::parse(R"({"text":"[Tease]","duration_ms":6000,
        "positive_styles":["a"],"negative_styles":["b"],"context_adherence":"high"})"));
    EXPECT_TRUE(creatures::voice::planChunkToSectionJson(
                    nlohmann::json::parse(R"({"song_id":"s","range":{"start_ms":0,"end_ms":5000}})"))
                    .is_null());
}
