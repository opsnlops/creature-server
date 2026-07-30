#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "server/config.h"
#include "server/rtp/AsyncAudioTraceContext.h"
#include "server/rtp/RtpClockMapping.h"

namespace creatures {
class OperationSpan;
}

namespace creatures::rtp {

/**
 * Periodically publishes RTCP Sender Reports for the multicast RTP channels.
 *
 * RTP output only updates a small in-memory snapshot under a mutex. Packet
 * construction and all socket work happen on this class's dedicated thread.
 */
class RtcpSender {
  public:
    using SynchronizationSources = std::array<uint32_t, RTP_STREAMING_CHANNELS>;
    using SentOctets = std::array<uint32_t, RTP_STREAMING_CHANNELS>;

    explicit RtcpSender(std::chrono::milliseconds reportInterval = std::chrono::milliseconds(RTCP_REPORT_INTERVAL_MS));
    ~RtcpSender();

    RtcpSender(const RtcpSender &) = delete;
    RtcpSender &operator=(const RtcpSender &) = delete;

    void beginSession(uint64_t generation, std::string ownerId, const SynchronizationSources &synchronizationSources,
                      const RtpClockMapping &clockMapping, AsyncAudioTraceContext traceContext);
    void recordFrame(uint64_t generation, const SentOctets &sentOctets);
    void endSession(uint64_t generation);
    void discardSessionUnless(uint64_t generation);
    void shutdown();

    [[nodiscard]] bool isReady() const { return ready_.load(); }
    [[nodiscard]] const std::string &canonicalName() const { return canonicalName_; }

  private:
    struct SessionState {
        uint64_t generation;
        std::string ownerId;
        SynchronizationSources synchronizationSources;
        RtpClockMapping clockMapping;
        AsyncAudioTraceContext traceContext;
        std::array<uint32_t, RTP_STREAMING_CHANNELS> packetCounts{};
        std::array<uint32_t, RTP_STREAMING_CHANNELS> octetCounts{};
        bool hasPackets{false};
        uint64_t reportCount{0};
    };

    struct ReportSnapshot {
        uint64_t generation;
        std::string ownerId;
        SynchronizationSources synchronizationSources;
        RtpClockMapping clockMapping;
        AsyncAudioTraceContext traceContext;
        std::array<uint32_t, RTP_STREAMING_CHANNELS> packetCounts;
        std::array<uint32_t, RTP_STREAMING_CHANNELS> octetCounts;
        bool initialReport;
    };

    [[nodiscard]] static std::string makeCanonicalName();
    [[nodiscard]] bool openSocket();
    void run();
    void sendReport(const ReportSnapshot &snapshot) noexcept;
    void recordFirstReport(const ReportSnapshot &snapshot, const RtpWallClockTimestamp &timestamp) noexcept;
    void recordSendFailure(const ReportSnapshot &snapshot, const RtpWallClockTimestamp &timestamp,
                           size_t failedChannels, uint8_t firstFailedChannel, int firstError) noexcept;
    static void addTraceContextAttributes(const std::shared_ptr<creatures::OperationSpan> &span,
                                          const ReportSnapshot &snapshot);

    const std::chrono::milliseconds reportInterval_;
    const std::string canonicalName_;
    int socket_{-1};
    std::atomic<bool> ready_{false};
    std::thread worker_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<SessionState> session_;
    uint64_t stateRevision_{0};
    bool immediateReportRequested_{false};
    bool stopping_{false};
    std::chrono::steady_clock::time_point nextReportTime_{};
};

} // namespace creatures::rtp
