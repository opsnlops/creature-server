#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "server/audio/SoundPathResolver.h"

namespace creatures::audio {
namespace {

namespace fs = std::filesystem;

// A temp sound root that mirrors the real permanent store: a few top-level
// sounds plus a dialog/ subdir of UUID-named renders (issue #46).
class SoundPathResolverTest : public ::testing::Test {
  protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("soundpath-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        fs::create_directories(root_ / "dialog");
        writeFile(root_ / "hello.wav");
        writeFile(root_ / "music.flac");
        dialogUuid_ = "3f2504e0-4f89-41d3-9a0c-0305e82c3301.wav";
        writeFile(root_ / "dialog" / dialogUuid_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    static void writeFile(const fs::path &p) {
        std::ofstream out(p, std::ios::binary);
        out << "RIFF....WAVE"; // contents don't matter for path resolution
    }

    fs::path root_;
    std::string dialogUuid_;
};

TEST_F(SoundPathResolverTest, ResolvesTopLevelSound) {
    auto r = resolveSoundInRoot(root_, "hello.wav");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(fs::path(*r), fs::canonical(root_ / "hello.wav"));
}

TEST_F(SoundPathResolverTest, ResolvesSoundInDialogSubdirByBasename) {
    // The whole point of #46: a dialog render addressed only by its basename
    // resolves to the file living under dialog/.
    auto r = resolveSoundInRoot(root_, dialogUuid_);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(fs::path(*r), fs::canonical(root_ / "dialog" / dialogUuid_));
}

TEST_F(SoundPathResolverTest, ReturnsNulloptForMissingSound) {
    auto r = resolveSoundInRoot(root_, "does-not-exist.wav");
    EXPECT_FALSE(r.has_value());
}

TEST_F(SoundPathResolverTest, ReturnsNulloptWhenRootMissing) {
    auto r = resolveSoundInRoot(root_ / "nope", "hello.wav");
    EXPECT_FALSE(r.has_value());
}

TEST_F(SoundPathResolverTest, TopLevelTakesPrecedenceOverSubdir) {
    // A basename that exists both at top level and in a subdir resolves to the
    // top-level file (the fast path), never the nested one.
    const std::string shared = "shared.wav";
    writeFile(root_ / shared);
    writeFile(root_ / "dialog" / shared);
    auto r = resolveSoundInRoot(root_, shared);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(fs::path(*r), fs::canonical(root_ / shared));
}

TEST_F(SoundPathResolverTest, DoesNotMatchDirectories) {
    // A directory whose name equals the query must not resolve as a file.
    fs::create_directories(root_ / "afolder.wav");
    auto r = resolveSoundInRoot(root_, "afolder.wav");
    EXPECT_FALSE(r.has_value());
}

TEST_F(SoundPathResolverTest, RejectsAbsoluteAndTraversalPaths) {
    EXPECT_FALSE(resolveSoundInRoot(root_, (root_ / "hello.wav").string()).has_value());
    EXPECT_FALSE(resolveSoundInRoot(root_, "../hello.wav").has_value());
    EXPECT_FALSE(resolveSoundInRoot(root_, "dialog/hello.wav").has_value());
}

TEST_F(SoundPathResolverTest, StoredSoundFilePathsMustBeReducedToTheirBasenameFirst) {
    // AnimationMetadata::sound_file is stored WITH its subdirectory, e.g.
    // "dialog/scene-abc123.wav". Handing that straight to this function is
    // rejected by the traversal guard above — correctly, but silently, since
    // the return is just nullopt.
    //
    // That is exactly how issue #145 happened: the stage re-render passed
    // `sound_file` unchanged, so its iXML lipsync recovery never once ran and
    // every dialog fell through to the lossy mouth-slot scrape. A caller
    // holding a stored path has to take .filename() first.
    const std::string stored = "dialog/" + dialogUuid_;
    EXPECT_FALSE(resolveSoundInRoot(root_, stored).has_value());

    auto r = resolveSoundInRoot(root_, fs::path(stored).filename().string());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(fs::path(*r), fs::canonical(root_ / "dialog" / dialogUuid_));
}

// --- SoundStoreIndex (issue #94) ---

using IndexStatus = SoundStoreIndex::Status;

TEST_F(SoundPathResolverTest, IndexFindsTopLevelAndSubdirSounds) {
    SoundStoreIndex index(root_);
    auto top = index.find("hello.wav");
    ASSERT_EQ(top.status, IndexStatus::Found);
    EXPECT_EQ(fs::path(top.entry->canonicalPath), fs::canonical(root_ / "hello.wav"));

    auto nested = index.find(dialogUuid_);
    ASSERT_EQ(nested.status, IndexStatus::Found);
    EXPECT_EQ(fs::path(nested.entry->canonicalPath), fs::canonical(root_ / "dialog" / dialogUuid_));
}

TEST_F(SoundPathResolverTest, IndexMissDoesNotWalkTheTree) {
    SoundStoreIndex index(root_);
    index.rebuildNow();

    // Plant a new file in a SUBDIR after the rebuild: a warm miss must not
    // discover it (that would mean a tree walk); only an invalidation may.
    writeFile(root_ / "dialog" / "late-arrival.wav");
    EXPECT_EQ(index.find("late-arrival.wav").status, IndexStatus::NotFound);

    index.markDirty();
    EXPECT_EQ(index.find("late-arrival.wav").status, IndexStatus::Found);
}

TEST_F(SoundPathResolverTest, IndexTopLevelWinsOverSubdirDeterministically) {
    // Preserves the store's documented behavior (issue #46): a top-level file
    // beats a subdir'd file of the same name — deterministically, not by
    // iteration order.
    const std::string shared = "shared.wav";
    writeFile(root_ / shared);
    writeFile(root_ / "dialog" / shared);

    SoundStoreIndex index(root_);
    auto lookup = index.find(shared);
    ASSERT_EQ(lookup.status, IndexStatus::Found);
    EXPECT_EQ(fs::path(lookup.entry->canonicalPath), fs::canonical(root_ / shared));
}

TEST_F(SoundPathResolverTest, IndexReportsSubdirDuplicatesAsDeterministicAmbiguity) {
    // The real-world duplicate: every streaming speech session writes s1.wav
    // into its own subdir. No natural winner exists, so this is a deterministic
    // ambiguity — the old resolver played whichever the iterator met first.
    fs::create_directories(root_ / "session-a");
    fs::create_directories(root_ / "session-b");
    writeFile(root_ / "session-a" / "s1.wav");
    writeFile(root_ / "session-b" / "s1.wav");

    SoundStoreIndex index(root_);
    auto first = index.find("s1.wav");
    ASSERT_EQ(first.status, IndexStatus::Ambiguous);
    ASSERT_EQ(first.candidates.size(), 2U);

    // Candidates are root-relative (safe to echo to clients) and sorted.
    EXPECT_EQ(first.candidates[0], (fs::path("session-a") / "s1.wav").string());
    EXPECT_EQ(first.candidates[1], (fs::path("session-b") / "s1.wav").string());

    auto second = index.find("s1.wav");
    ASSERT_EQ(second.status, IndexStatus::Ambiguous);
    EXPECT_EQ(first.candidates, second.candidates);
}

TEST_F(SoundPathResolverTest, IndexSelfHealsWhenAFileVanishes) {
    SoundStoreIndex index(root_);
    ASSERT_EQ(index.find("hello.wav").status, IndexStatus::Found);

    fs::remove(root_ / "hello.wav");
    EXPECT_EQ(index.find("hello.wav").status, IndexStatus::NotFound)
        << "a stale entry must never be served for a vanished file";
}

TEST_F(SoundPathResolverTest, IndexProbeFindsNewTopLevelFileWithoutRebuild) {
    SoundStoreIndex index(root_);
    index.rebuildNow();

    // Out-of-band writers drop files at the top level without invalidating;
    // the single-file probe (not a walk) picks those up.
    writeFile(root_ / "surprise.wav");
    auto lookup = index.find("surprise.wav");
    ASSERT_EQ(lookup.status, IndexStatus::Found);
    EXPECT_EQ(fs::path(lookup.entry->canonicalPath), fs::canonical(root_ / "surprise.wav"));
}

TEST_F(SoundPathResolverTest, IndexAmbiguityHealsToFoundWhenOneCandidateVanishes) {
    fs::create_directories(root_ / "session-a");
    fs::create_directories(root_ / "session-b");
    writeFile(root_ / "session-a" / "s1.wav");
    writeFile(root_ / "session-b" / "s1.wav");

    SoundStoreIndex index(root_);
    ASSERT_EQ(index.find("s1.wav").status, IndexStatus::Ambiguous);

    fs::remove(root_ / "session-b" / "s1.wav");
    auto healed = index.find("s1.wav");
    ASSERT_EQ(healed.status, IndexStatus::Found);
    EXPECT_EQ(fs::path(healed.entry->canonicalPath), fs::canonical(root_ / "session-a" / "s1.wav"));
}

TEST_F(SoundPathResolverTest, IndexDropsChangedEntryWhoseRefreshEscapesTheRoot) {
    // A file replaced by a symlink whose target escapes the root must be
    // dropped, not served stale — the containment boundary holds even through
    // self-healing (issue #94 review).
    const auto outside = fs::temp_directory_path() / ("outside-" + std::to_string(::getpid()) + ".wav");
    {
        std::ofstream out(outside, std::ios::binary);
        out << "RIFF....WAVEoutside-data-differs";
    }

    SoundStoreIndex index(root_);
    ASSERT_EQ(index.find("hello.wav").status, IndexStatus::Found);

    fs::remove(root_ / "hello.wav");
    fs::create_symlink(outside, root_ / "hello.wav");

    auto lookup = index.find("hello.wav");
    EXPECT_NE(lookup.status, IndexStatus::Found) << "a symlink escaping the root must never be served";

    std::error_code ec;
    fs::remove(outside, ec);
}

TEST_F(SoundPathResolverTest, IndexSkipsDotDirectories) {
    // The Opus packet cache lives at .opus_cache/<hostname>/ inside the sound
    // root; its files must never be resolvable by basename.
    fs::create_directories(root_ / ".opus_cache" / "testhost");
    writeFile(root_ / ".opus_cache" / "testhost" / "ch00.opus");

    SoundStoreIndex index(root_);
    EXPECT_EQ(index.find("ch00.opus").status, IndexStatus::NotFound);
}

TEST_F(SoundPathResolverTest, IndexRejectsPathlikeQueries) {
    SoundStoreIndex index(root_);
    EXPECT_EQ(index.find((root_ / "hello.wav").string()).status, IndexStatus::NotFound);
    EXPECT_EQ(index.find("../hello.wav").status, IndexStatus::NotFound);
    EXPECT_EQ(index.find("dialog/hello.wav").status, IndexStatus::NotFound);
    EXPECT_EQ(index.find("").status, IndexStatus::NotFound);
}

TEST_F(SoundPathResolverTest, IndexHandlesMissingRoot) {
    SoundStoreIndex index(root_ / "does-not-exist");
    EXPECT_EQ(index.find("hello.wav").status, IndexStatus::NotFound);
    EXPECT_EQ(index.entryCount(), 0U);
}

// --- filename validation (issue #94) ---

TEST(SoundFilenameValidation, AcceptsOrdinaryNames) {
    EXPECT_TRUE(isSafeSoundFilename("hello.wav"));
    EXPECT_TRUE(isSafeSoundFilename("3f2504e0-4f89-41d3-9a0c-0305e82c3301.wav"));
    EXPECT_TRUE(isSafeSoundFilename("Beaky's Big Number!.wav"));
}

TEST(SoundFilenameValidation, AcceptsWellFormedUtf8) {
    // Non-ASCII names are legitimate; their continuation bytes live in
    // 0x80-0xBF and must not be mistaken for C1 controls.
    EXPECT_TRUE(isSafeSoundFilename("café.wav"));
    EXPECT_TRUE(isSafeSoundFilename("うた.wav"));
}

TEST(SoundFilenameValidation, RejectsControlCharacters) {
    EXPECT_FALSE(isSafeSoundFilename(std::string("bad\nname.wav")));
    EXPECT_FALSE(isSafeSoundFilename(std::string("bad\x1b[31mname.wav"))); // ANSI escape
    EXPECT_FALSE(isSafeSoundFilename(std::string("del\x7f.wav")));
    // C1 (U+0085 NEL) encoded as UTF-8: 0xC2 0x85
    EXPECT_FALSE(isSafeSoundFilename(std::string("nel\xc2\x85.wav")));
}

TEST(SoundFilenameValidation, RejectsMalformedUtf8) {
    EXPECT_FALSE(isSafeSoundFilename(std::string("lone\x85.wav")));         // bare continuation
    EXPECT_FALSE(isSafeSoundFilename(std::string("overlong\xc0\xaf.wav"))); // overlong '/'
    EXPECT_FALSE(isSafeSoundFilename(std::string("trunc\xc3")));            // truncated sequence
}

TEST(SoundFilenameValidation, RejectsPathsAndOversizedNames) {
    EXPECT_FALSE(isSafeSoundFilename(""));
    EXPECT_FALSE(isSafeSoundFilename("../evil.wav"));
    EXPECT_FALSE(isSafeSoundFilename("/etc/passwd"));
    EXPECT_FALSE(isSafeSoundFilename("dialog/x.wav"));
    EXPECT_FALSE(isSafeSoundFilename(std::string(256, 'a') + ".wav"));
    EXPECT_TRUE(isSafeSoundFilename(std::string(251, 'a') + ".wav")); // exactly 255 bytes
}

TEST(SoundFilenameValidation, SanitizerBoundsAndEscapes) {
    EXPECT_EQ(sanitizeForLogging("hello.wav"), "hello.wav");
    EXPECT_EQ(sanitizeForLogging(std::string("bad\x1bname")), "bad\\x1bname");
    const auto longName = std::string(200, 'a');
    const auto sanitized = sanitizeForLogging(longName);
    EXPECT_LT(sanitized.size(), longName.size());
}

} // namespace
} // namespace creatures::audio
