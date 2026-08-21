#include <cctype>
#include <gtest/gtest.h>

#include "util/Slugify.h"

TEST(Slugify, ProducesReadableSafeComponent) {
    EXPECT_EQ(creatures::util::slugify("Mysterious, Playful Orchestra!", 40, "music"), "mysterious-playful-orchestra");
}

TEST(Slugify, AppliesLimitAndFallback) {
    EXPECT_EQ(creatures::util::slugify("abcdef", 3, "music"), "abc");
    EXPECT_EQ(creatures::util::slugify("!!!", 40, "music"), "music");
}

// ===========================================================================
// The shared export-name shape (#126, #152): "{slug}-{short id}". One helper
// so dialog renders, voice takes, BGM exports, and exchange downloads can't
// drift apart.
// ===========================================================================

TEST(ExportBasename, SlugPlusEightCharIdTail) {
    EXPECT_EQ(creatures::util::exportBasename("The Great Seed Heist", "a1b2c3d4-e5f6-7890-abcd-ef0123456789"),
              "the-great-seed-heist-a1b2c3d4");
}

TEST(ExportBasename, HonorsCustomFallbackAndTailLength) {
    EXPECT_EQ(creatures::util::exportBasename("!!!", "a1b2c3d4e5f6", 48, "exchange", 12), "exchange-a1b2c3d4e5f6");
}

TEST(ExportBasename, ToleratesAnIdShorterThanTheTail) {
    EXPECT_EQ(creatures::util::exportBasename("Beep", "ab"), "beep-ab");
}

TEST(BgmExportBasename, TripleAgreesBetweenWavAndMp3Sites) {
    // The exact shape both DialogMusicService (WAV) and DialogMusicController
    // (MP3 download) must produce for the same generation.
    EXPECT_EQ(creatures::util::bgmExportBasename("Scene 3 — Travel", "Mysterious, Playful Orchestra!",
                                                 "0123456789abcdef0123"),
              "scene-3-travel--bgm--mysterious-playful-orchestra--0123456789ab");
}

// ===========================================================================
// Dialog audio filenames (issue #126)
//
// Rendered dialog WAVs are named from the render title rather than the job
// UUID. The name is the only label the file carries once it leaves the
// system — in the sound store, the Console's list, and the shared MP3
// rendition that derives from this basename. These pin the cases real titles
// actually hit.
// ===========================================================================

TEST(SlugifyDialogTitleTest, HandlesTheEmDashesTitlesActuallyContain) {
    // The stage suffix is joined with an em dash, so every stage-bound render
    // hits this.
    EXPECT_EQ(creatures::util::slugify("Scene 3 — Travel", 48, "dialog"), "scene-3-travel");
}

TEST(SlugifyDialogTitleTest, HandlesPunctuationAuthorsType) {
    EXPECT_EQ(creatures::util::slugify("April's \"Big\" Talk: MFM2024 / 6-2-1 Rule", 48, "dialog"),
              "april-s-big-talk-mfm2024-6-2-1-rule");
}

TEST(SlugifyDialogTitleTest, FallsBackWhenNothingSurvives) {
    // A whitespace-only or entirely non-ASCII title still has to produce a
    // usable filename — the job-id tail keeps it unique.
    EXPECT_EQ(creatures::util::slugify("   ", 48, "dialog"), "dialog");
    EXPECT_EQ(creatures::util::slugify("日本語のタイトル", 48, "dialog"), "dialog");
}

TEST(SlugifyDialogTitleTest, IsBoundedSoFilenamesStayReasonable) {
    const auto slug = creatures::util::slugify(
        "A very long title that goes on and on and should be truncated somewhere sensible indeed", 48, "dialog");
    EXPECT_LE(slug.size(), 48u);
    EXPECT_FALSE(slug.empty());
}

TEST(SlugifyDialogTitleTest, ProducesFilesystemSafeOutput) {
    // Whatever goes in, the result has to be safe as a path component.
    for (const auto *title : {"Scene 3 — Travel", "a/b\\c", "..", "with\ttabs\nand newlines",
                              "April's \"Big\" Talk: MFM2024 / 6-2-1 Rule"}) {
        const auto slug = creatures::util::slugify(title, 48, "dialog");
        EXPECT_FALSE(slug.empty()) << title;
        EXPECT_EQ(slug.find('/'), std::string::npos) << title;
        EXPECT_EQ(slug.find('\\'), std::string::npos) << title;
        EXPECT_NE(slug, ".") << title;
        EXPECT_NE(slug, "..") << title;
        for (const unsigned char ch : slug) {
            EXPECT_TRUE(std::isalnum(ch) || ch == '-') << "'" << slug << "' from '" << title << "'";
        }
    }
}
