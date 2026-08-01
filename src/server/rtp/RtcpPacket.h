#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace creatures::rtp {

struct RtcpSenderReportData {
    uint32_t synchronizationSource{0};
    uint64_t ntpTimestamp{0};
    uint32_t rtpTimestamp{0};
    uint32_t packetCount{0};
    uint32_t octetCount{0};
    std::string_view canonicalName;
};

/**
 * Build an RFC 3550 compound packet containing one Sender Report followed by
 * the mandatory SDES CNAME chunk for the same SSRC.
 */
[[nodiscard]] std::vector<uint8_t> buildRtcpSenderReport(const RtcpSenderReportData &report);

} // namespace creatures::rtp
