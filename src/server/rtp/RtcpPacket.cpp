#include "server/rtp/RtcpPacket.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace creatures::rtp {
namespace {

constexpr uint8_t RTCP_VERSION = 2;
constexpr uint8_t RTCP_SENDER_REPORT = 200;
constexpr uint8_t RTCP_SOURCE_DESCRIPTION = 202;
constexpr uint8_t RTCP_SDES_CNAME = 1;
constexpr size_t RTCP_SENDER_REPORT_SIZE = 28;

void appendU16(std::vector<uint8_t> &packet, uint16_t value) {
    packet.push_back(static_cast<uint8_t>(value >> 8U));
    packet.push_back(static_cast<uint8_t>(value));
}

void appendU32(std::vector<uint8_t> &packet, uint32_t value) {
    packet.push_back(static_cast<uint8_t>(value >> 24U));
    packet.push_back(static_cast<uint8_t>(value >> 16U));
    packet.push_back(static_cast<uint8_t>(value >> 8U));
    packet.push_back(static_cast<uint8_t>(value));
}

} // namespace

std::vector<uint8_t> buildRtcpSenderReport(const RtcpSenderReportData &report) {
    if (report.synchronizationSource == 0) {
        throw std::invalid_argument("RTCP Sender Report requires a non-zero SSRC");
    }
    if (report.canonicalName.empty() || report.canonicalName.size() > UINT8_MAX) {
        throw std::invalid_argument("RTCP SDES CNAME must contain between 1 and 255 bytes");
    }

    const size_t unpaddedSdesChunkSize =
        sizeof(uint32_t) + sizeof(uint8_t) * 2 + report.canonicalName.size() + sizeof(uint8_t);
    const size_t paddedSdesChunkSize = (unpaddedSdesChunkSize + 3U) & ~size_t{3U};
    const size_t sdesPacketSize = sizeof(uint32_t) + paddedSdesChunkSize;

    std::vector<uint8_t> packet;
    packet.reserve(RTCP_SENDER_REPORT_SIZE + sdesPacketSize);

    // Sender Report: V=2, P=0, RC=0, length=6 32-bit words after the header.
    packet.push_back(static_cast<uint8_t>(RTCP_VERSION << 6U));
    packet.push_back(RTCP_SENDER_REPORT);
    appendU16(packet, 6);
    appendU32(packet, report.synchronizationSource);
    appendU32(packet, static_cast<uint32_t>(report.ntpTimestamp >> 32U));
    appendU32(packet, static_cast<uint32_t>(report.ntpTimestamp));
    appendU32(packet, report.rtpTimestamp);
    appendU32(packet, report.packetCount);
    appendU32(packet, report.octetCount);

    // SDES: V=2, P=0, SC=1. The length includes the padded CNAME chunk.
    packet.push_back(static_cast<uint8_t>((RTCP_VERSION << 6U) | 1U));
    packet.push_back(RTCP_SOURCE_DESCRIPTION);
    appendU16(packet, static_cast<uint16_t>(sdesPacketSize / sizeof(uint32_t) - 1U));
    appendU32(packet, report.synchronizationSource);
    packet.push_back(RTCP_SDES_CNAME);
    packet.push_back(static_cast<uint8_t>(report.canonicalName.size()));
    packet.insert(packet.end(), report.canonicalName.begin(), report.canonicalName.end());
    packet.push_back(0); // END item
    while (packet.size() % sizeof(uint32_t) != 0) {
        packet.push_back(0);
    }

    return packet;
}

} // namespace creatures::rtp
