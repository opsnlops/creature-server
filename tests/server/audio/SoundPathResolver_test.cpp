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

} // namespace
} // namespace creatures::audio
