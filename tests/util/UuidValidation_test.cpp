#include <gtest/gtest.h>

#include "util/UuidValidation.h"

namespace creatures {
namespace {

TEST(UuidValidation, AcceptsCanonicalUpperAndLowerCaseHex) {
    EXPECT_TRUE(isUuidShape("00000000-0000-4000-8000-000000000001"));
    EXPECT_TRUE(isUuidShape("ABCDEF12-3456-4ABC-8DEF-1234567890AB"));
}

TEST(UuidValidation, RejectsWrongLengthDashesAndNonHexCharacters) {
    EXPECT_FALSE(isUuidShape("00000000-0000-4000-8000-00000000001"));
    EXPECT_FALSE(isUuidShape("000000000000-4000-8000-000000000001"));
    EXPECT_FALSE(isUuidShape("00000000-0000-4000-8000-00000000000g"));
}

} // namespace
} // namespace creatures
