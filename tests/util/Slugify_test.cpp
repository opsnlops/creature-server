#include <gtest/gtest.h>

#include "util/Slugify.h"

TEST(Slugify, ProducesReadableSafeComponent) {
    EXPECT_EQ(creatures::util::slugify("Mysterious, Playful Orchestra!", 40, "music"), "mysterious-playful-orchestra");
}

TEST(Slugify, AppliesLimitAndFallback) {
    EXPECT_EQ(creatures::util::slugify("abcdef", 3, "music"), "abc");
    EXPECT_EQ(creatures::util::slugify("!!!", 40, "music"), "music");
}
