#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/DialogCache.h"
#include "server/voice/DialogClient.h"

using creatures::voice::CachedGeneration;
using creatures::voice::computeCacheKey;
using creatures::voice::DialogInput;
using creatures::voice::DialogWavProvenance;
using creatures::voice::findLatestGeneration;
using creatures::voice::ForcedAlignmentChar;
using creatures::voice::ForcedAlignmentResult;
using creatures::voice::ForcedAlignmentWord;
using creatures::voice::listGenerations;
using creatures::voice::loadGeneration;
using creatures::voice::saveGeneration;
using creatures::voice::updateGenerationProvenance;

namespace {

std::filesystem::path cacheDirFor(const std::string &cacheKey) {
    return std::filesystem::temp_directory_path() / "creature-adhoc" / "dialog-cache" / cacheKey;
}

/// RAII helper that wipes the cache directory for whatever cache_keys a test
/// touched, so tests don't leave artifacts behind or step on each other.
class CacheScope {
  public:
    void track(const std::string &cacheKey) { keys_.push_back(cacheKey); }
    ~CacheScope() {
        std::error_code ec;
        for (const auto &k : keys_) {
            std::filesystem::remove_all(cacheDirFor(k), ec);
        }
    }

  private:
    std::vector<std::string> keys_;
};

DialogInput turn(std::string v, std::string t) { return {std::move(v), std::move(t)}; }

CachedGeneration sampleGen(const std::string &id, std::vector<uint8_t> audio = {1, 2, 3, 4}) {
    CachedGeneration g;
    g.generationId = id;
    g.audioPcm = std::move(audio);
    g.createdAt = std::chrono::system_clock::now();
    g.turnsSummary = "test summary";
    g.voiceSegments.push_back({"voice-A", 0, 5, 0, 0.0, 1.0});
    g.forcedAlignment.loss = 0.05;
    g.forcedAlignment.words.push_back({"hi", 0.0, 0.5});
    g.forcedAlignment.characters.push_back({"h", 0.0, 0.25});
    g.forcedAlignment.characters.push_back({"i", 0.25, 0.5});
    return g;
}

} // namespace

TEST(DialogCacheComputeKey, StableAcrossCallsWithSameInput) {
    std::vector<DialogInput> a{turn("voice-A", "hello"), turn("voice-B", "world")};
    std::vector<DialogInput> b{turn("voice-A", "hello"), turn("voice-B", "world")};
    EXPECT_EQ(computeCacheKey(a), computeCacheKey(b));
    EXPECT_EQ(computeCacheKey(a).size(), 64u) << "sha256 hex is 64 chars";
}

TEST(DialogCacheComputeKey, DifferentForDifferentText) {
    std::vector<DialogInput> a{turn("v", "hello")};
    std::vector<DialogInput> b{turn("v", "world")};
    EXPECT_NE(computeCacheKey(a), computeCacheKey(b));
}

TEST(DialogCacheComputeKey, DifferentForDifferentVoice) {
    std::vector<DialogInput> a{turn("voice-A", "hello")};
    std::vector<DialogInput> b{turn("voice-B", "hello")};
    EXPECT_NE(computeCacheKey(a), computeCacheKey(b));
}

TEST(DialogCacheComputeKey, OrderMatters) {
    std::vector<DialogInput> a{turn("v1", "first"), turn("v2", "second")};
    std::vector<DialogInput> b{turn("v2", "second"), turn("v1", "first")};
    EXPECT_NE(computeCacheKey(a), computeCacheKey(b)) << "turn order changes the dialog → must change the key";
}

TEST(DialogCacheSaveLoad, RoundTripsAllFields) {
    CacheScope scope;
    std::vector<DialogInput> turns{turn("voice-test-A", "DialogCacheSaveLoad-RoundTripsAllFields")};
    const auto key = computeCacheKey(turns);
    scope.track(key);

    CachedGeneration in = sampleGen("gen-rt-1", {10, 20, 30, 40, 50, 60});
    auto saveRes = saveGeneration(key, in);
    ASSERT_TRUE(saveRes.isSuccess()) << (saveRes.getError() ? saveRes.getError().value().getMessage() : "");

    auto loadRes = loadGeneration(key, "gen-rt-1");
    ASSERT_TRUE(loadRes.isSuccess()) << (loadRes.getError() ? loadRes.getError().value().getMessage() : "");
    const auto out = loadRes.getValue().value();

    EXPECT_EQ(out.generationId, "gen-rt-1");
    EXPECT_EQ(out.audioPcm, in.audioPcm);
    EXPECT_EQ(out.turnsSummary, in.turnsSummary);
    ASSERT_EQ(out.voiceSegments.size(), 1u);
    EXPECT_EQ(out.voiceSegments[0].voiceId, "voice-A");
    EXPECT_EQ(out.voiceSegments[0].characterStartIndex, 0u);
    EXPECT_EQ(out.voiceSegments[0].characterEndIndex, 5u);
    EXPECT_DOUBLE_EQ(out.forcedAlignment.loss, 0.05);
    ASSERT_EQ(out.forcedAlignment.words.size(), 1u);
    EXPECT_EQ(out.forcedAlignment.words[0].text, "hi");
    EXPECT_DOUBLE_EQ(out.forcedAlignment.words[0].startSeconds, 0.0);
    EXPECT_DOUBLE_EQ(out.forcedAlignment.words[0].endSeconds, 0.5);
    ASSERT_EQ(out.forcedAlignment.characters.size(), 2u);
    EXPECT_EQ(out.forcedAlignment.characters[0].text, "h");
}

TEST(DialogCacheSaveLoad, RoundTripsProvenance) {
    CacheScope scope;
    std::vector<DialogInput> turns{turn("voice-prov", "DialogCacheSaveLoad-RoundTripsProvenance")};
    const auto key = computeCacheKey(turns);
    scope.track(key);

    CachedGeneration in = sampleGen("gen-prov-1");
    in.provenance.sourceScriptId = "script-42";
    in.provenance.title = "Provenance Scene";
    in.provenance.generationIds = {"gen-prov-1"};
    in.provenance.tracks = {{1, "Beaky"}, {2, "Pip"}, {17, "BGM"}};
    in.provenance.script = {{"Beaky", "hello & <friends>"}, {"Pip", "web scale"}};
    ASSERT_TRUE(saveGeneration(key, in).isSuccess());

    auto loadRes = loadGeneration(key, "gen-prov-1");
    ASSERT_TRUE(loadRes.isSuccess());
    const auto loaded = loadRes.getValue().value();
    const auto &p = loaded.provenance;
    EXPECT_EQ(p.sourceScriptId, "script-42");
    EXPECT_EQ(p.title, "Provenance Scene");
    EXPECT_EQ(p.generationIds, std::vector<std::string>{"gen-prov-1"});
    ASSERT_EQ(p.tracks.size(), 3u);
    EXPECT_EQ(p.tracks[0].channel, 1);
    EXPECT_EQ(p.tracks[0].name, "Beaky");
    EXPECT_EQ(p.tracks[2].name, "BGM");
    ASSERT_EQ(p.script.size(), 2u);
    EXPECT_EQ(p.script[0].speaker, "Beaky");
    EXPECT_EQ(p.script[0].text, "hello & <friends>"); // raw text; JSON handles escaping
}

TEST(DialogCacheUpdateProvenance, RewritesJsonAndLeavesAudioIntact) {
    CacheScope scope;
    std::vector<DialogInput> turns{turn("voice-upd", "DialogCacheUpdateProvenance-RewritesJson")};
    const auto key = computeCacheKey(turns);
    scope.track(key);

    // Save with no provenance (mirrors a generation cached before provenance is known).
    CachedGeneration in = sampleGen("gen-upd-1", {9, 8, 7, 6, 5, 4});
    ASSERT_TRUE(saveGeneration(key, in).isSuccess());
    ASSERT_TRUE(loadGeneration(key, "gen-upd-1").getValue().value().provenance.script.empty());

    DialogWavProvenance prov;
    prov.title = "Stamped Later";
    prov.script = {{"Beaky", "added after the fact"}};
    ASSERT_TRUE(updateGenerationProvenance(key, "gen-upd-1", prov).isSuccess());

    const auto reloaded = loadGeneration(key, "gen-upd-1");
    ASSERT_TRUE(reloaded.isSuccess());
    EXPECT_EQ(reloaded.getValue().value().provenance.title, "Stamped Later");
    ASSERT_EQ(reloaded.getValue().value().provenance.script.size(), 1u);
    EXPECT_EQ(reloaded.getValue().value().provenance.script[0].text, "added after the fact");
    // Audio untouched by the json-only update.
    EXPECT_EQ(reloaded.getValue().value().audioPcm, in.audioPcm);
}

TEST(DialogCacheUpdateProvenance, NotFoundForMissingGeneration) {
    CacheScope scope;
    std::vector<DialogInput> turns{turn("v", "DialogCacheUpdateProvenance-NotFound")};
    const auto key = computeCacheKey(turns);
    scope.track(key);

    DialogWavProvenance prov;
    prov.title = "x";
    auto res = updateGenerationProvenance(key, "no-such-gen", prov);
    ASSERT_FALSE(res.isSuccess());
    EXPECT_EQ(res.getError().value().getCode(), creatures::ServerError::NotFound);
}

TEST(DialogCacheLoad, NotFoundOnMissingGeneration) {
    CacheScope scope;
    std::vector<DialogInput> turns{turn("v", "DialogCacheLoad-NotFoundOnMissingGeneration")};
    const auto key = computeCacheKey(turns);
    scope.track(key);

    auto res = loadGeneration(key, "no-such-gen");
    ASSERT_FALSE(res.isSuccess());
    EXPECT_EQ(res.getError().value().getCode(), creatures::ServerError::NotFound);
}

TEST(DialogCacheList, EmptyWhenNothingCached) {
    std::vector<DialogInput> turns{turn("v", "DialogCacheList-EmptyWhenNothingCached-unique")};
    const auto key = computeCacheKey(turns);
    // Deliberately don't track or save — just confirm an unknown key returns empty.
    const auto gens = listGenerations(key);
    EXPECT_TRUE(gens.empty());
    EXPECT_FALSE(findLatestGeneration(key).has_value());
}

TEST(DialogCacheList, MultipleGenerationsSortedNewestFirst) {
    CacheScope scope;
    std::vector<DialogInput> turns{turn("v", "DialogCacheList-MultipleGenerationsSortedNewestFirst")};
    const auto key = computeCacheKey(turns);
    scope.track(key);

    // Save three generations with mtime spacing so the sort is deterministic.
    // sleep_for is acceptable here — filesystem mtime resolution is typically
    // 1 second on macOS; ~50ms wait keeps it ordered without flake.
    ASSERT_TRUE(saveGeneration(key, sampleGen("gen-oldest")).isSuccess());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(saveGeneration(key, sampleGen("gen-middle")).isSuccess());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(saveGeneration(key, sampleGen("gen-newest")).isSuccess());

    const auto gens = listGenerations(key);
    ASSERT_EQ(gens.size(), 3u);
    EXPECT_EQ(gens[0].generationId, "gen-newest");
    EXPECT_EQ(gens[2].generationId, "gen-oldest");

    const auto latest = findLatestGeneration(key);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(*latest, "gen-newest");
}

TEST(DialogCacheSave, RejectsEmptyCacheKey) {
    auto res = saveGeneration("", sampleGen("any"));
    ASSERT_FALSE(res.isSuccess());
    EXPECT_EQ(res.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST(DialogCacheSave, RejectsEmptyGenerationId) {
    CacheScope scope;
    std::vector<DialogInput> turns{turn("v", "DialogCacheSave-RejectsEmptyGenerationId")};
    const auto key = computeCacheKey(turns);
    scope.track(key);

    auto res = saveGeneration(key, sampleGen(""));
    ASSERT_FALSE(res.isSuccess());
    EXPECT_EQ(res.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST(DialogCacheSave, RejectsEmptyAudio) {
    CacheScope scope;
    std::vector<DialogInput> turns{turn("v", "DialogCacheSave-RejectsEmptyAudio")};
    const auto key = computeCacheKey(turns);
    scope.track(key);

    CachedGeneration g;
    g.generationId = "some-id";
    g.createdAt = std::chrono::system_clock::now();
    // audioPcm intentionally empty
    auto res = saveGeneration(key, g);
    ASSERT_FALSE(res.isSuccess());
    EXPECT_EQ(res.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST(DialogCacheList, OrphanJsonWithoutPcmIsIgnored) {
    CacheScope scope;
    std::vector<DialogInput> turns{turn("v", "DialogCacheList-OrphanJsonWithoutPcmIsIgnored")};
    const auto key = computeCacheKey(turns);
    scope.track(key);

    // Save a real one so the directory exists.
    ASSERT_TRUE(saveGeneration(key, sampleGen("real-gen")).isSuccess());

    // Drop an orphan .json with no matching .pcm next to it. listGenerations
    // should NOT count it (requires both files — protects loaders from a
    // half-written save being mistaken for complete).
    const auto dir = cacheDirFor(key);
    {
        std::ofstream orphan(dir / "orphan.json");
        orphan << "{}";
    }

    const auto gens = listGenerations(key);
    ASSERT_EQ(gens.size(), 1u) << "only the real generation should count";
    EXPECT_EQ(gens[0].generationId, "real-gen");
}
