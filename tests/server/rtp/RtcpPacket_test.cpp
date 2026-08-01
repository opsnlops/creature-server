#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/rtp/RtcpPacket.h"

namespace creatures::rtp {
namespace {

uint16_t readU16(const std::vector<uint8_t> &packet, size_t offset) {
    return static_cast<uint16_t>((static_cast<uint16_t>(packet.at(offset)) << 8U) | packet.at(offset + 1));
}

uint32_t readU32(const std::vector<uint8_t> &packet, size_t offset) {
    return (static_cast<uint32_t>(packet.at(offset)) << 24U) | (static_cast<uint32_t>(packet.at(offset + 1)) << 16U) |
           (static_cast<uint32_t>(packet.at(offset + 2)) << 8U) | packet.at(offset + 3);
}

TEST(RtcpPacket, BuildsSenderReportAndSdesCnameCompoundPacket) {
    const auto packet = buildRtcpSenderReport({
        .synchronizationSource = 0x0102'0304U,
        .ntpTimestamp = 0x1122'3344'5566'7788ULL,
        .rtpTimestamp = 0x99aa'bbccU,
        .packetCount = 123U,
        .octetCount = 45'678U,
        .canonicalName = "creature-server@test",
    });

    ASSERT_EQ(packet.size() % sizeof(uint32_t), 0U);
    ASSERT_GE(packet.size(), 40U);

    EXPECT_EQ(packet[0], 0x80U);
    EXPECT_EQ(packet[1], 200U);
    EXPECT_EQ(readU16(packet, 2), 6U);
    EXPECT_EQ(readU32(packet, 4), 0x0102'0304U);
    EXPECT_EQ(readU32(packet, 8), 0x1122'3344U);
    EXPECT_EQ(readU32(packet, 12), 0x5566'7788U);
    EXPECT_EQ(readU32(packet, 16), 0x99aa'bbccU);
    EXPECT_EQ(readU32(packet, 20), 123U);
    EXPECT_EQ(readU32(packet, 24), 45'678U);

    constexpr size_t sdesOffset = 28;
    EXPECT_EQ(packet[sdesOffset], 0x81U);
    EXPECT_EQ(packet[sdesOffset + 1], 202U);
    EXPECT_EQ((static_cast<size_t>(readU16(packet, sdesOffset + 2)) + 1U) * sizeof(uint32_t),
              packet.size() - sdesOffset);
    EXPECT_EQ(readU32(packet, sdesOffset + 4), 0x0102'0304U);
    EXPECT_EQ(packet[sdesOffset + 8], 1U);
    ASSERT_EQ(packet[sdesOffset + 9], 20U);
    EXPECT_EQ(std::string(packet.begin() + static_cast<std::ptrdiff_t>(sdesOffset + 10),
                          packet.begin() + static_cast<std::ptrdiff_t>(sdesOffset + 30)),
              "creature-server@test");
    EXPECT_EQ(packet[sdesOffset + 30], 0U);
}

TEST(RtcpPacket, KeepsSenderCountersIndependentPerReport) {
    const auto first = buildRtcpSenderReport({
        .synchronizationSource = 1000U,
        .ntpTimestamp = 10U,
        .rtpTimestamp = 20U,
        .packetCount = 30U,
        .octetCount = 40U,
        .canonicalName = "shared",
    });
    const auto second = buildRtcpSenderReport({
        .synchronizationSource = 1001U,
        .ntpTimestamp = 10U,
        .rtpTimestamp = 20U,
        .packetCount = 300U,
        .octetCount = 400U,
        .canonicalName = "shared",
    });

    EXPECT_EQ(readU32(first, 20), 30U);
    EXPECT_EQ(readU32(first, 24), 40U);
    EXPECT_EQ(readU32(second, 20), 300U);
    EXPECT_EQ(readU32(second, 24), 400U);
    EXPECT_EQ(readU32(first, 8), readU32(second, 8));
    EXPECT_EQ(readU32(first, 12), readU32(second, 12));
    EXPECT_EQ(readU32(first, 16), readU32(second, 16));
}

TEST(RtcpPacket, RejectsInvalidIdentityFields) {
    EXPECT_THROW(static_cast<void>(buildRtcpSenderReport({.synchronizationSource = 0, .canonicalName = "shared"})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(buildRtcpSenderReport({.synchronizationSource = 1, .canonicalName = ""})),
                 std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(buildRtcpSenderReport({.synchronizationSource = 1, .canonicalName = std::string(256, 'x')})),
        std::invalid_argument);
}

} // namespace
} // namespace creatures::rtp
