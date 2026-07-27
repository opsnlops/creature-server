#include <gtest/gtest.h>

#include "server/rtp/StandaloneRtpAdmission.h"

namespace creatures::rtp {

TEST(StandaloneRtpAdmission, AllowsOnlyOneLoaderReservation) {
    StandaloneRtpAdmission admission;
    auto first = admission.tryAcquire();

    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(admission.isReserved());
    EXPECT_EQ(admission.tryAcquire(), nullptr);

    first.reset();

    EXPECT_FALSE(admission.isReserved());
    EXPECT_NE(admission.tryAcquire(), nullptr);
}

TEST(StandaloneRtpAdmission, SharedCopiesHoldReservationUntilLastOwnerReleases) {
    StandaloneRtpAdmission admission;
    auto first = admission.tryAcquire();
    auto copy = first;

    first.reset();
    EXPECT_TRUE(admission.isReserved());
    EXPECT_EQ(admission.tryAcquire(), nullptr);

    copy.reset();
    EXPECT_FALSE(admission.isReserved());
}

TEST(StandaloneRtpAdmission, RequestFloodCannotCreatePendingReservations) {
    StandaloneRtpAdmission admission;
    auto active = admission.tryAcquire();
    ASSERT_NE(active, nullptr);

    for (size_t request = 0; request < 10'000; ++request) {
        EXPECT_EQ(admission.tryAcquire(), nullptr);
    }

    EXPECT_TRUE(admission.isReserved());
}

} // namespace creatures::rtp
