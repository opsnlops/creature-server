#include "server/rtp/RtcpSender.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include <fmt/format.h>

#include "server/metrics/counters.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/RtcpPacket.h"
#include "util/ObservabilityManager.h"
#include "util/threadName.h"

namespace creatures {
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::rtp {

RtcpSender::RtcpSender(std::chrono::milliseconds reportInterval)
    : reportInterval_(reportInterval), canonicalName_(makeCanonicalName()) {
    if (reportInterval_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("RTCP report interval must be positive");
    }
    if (openSocket()) {
        worker_ = std::thread(&RtcpSender::run, this);
        info("RTCP sender ready on port {} with CNAME '{}'", RTCP_PORT, canonicalName_);
    } else {
        error("RTCP sender unavailable on port {}; RTP audio will continue without synchronized timing", RTCP_PORT);
    }
}

RtcpSender::~RtcpSender() { shutdown(); }

void RtcpSender::beginSession(uint64_t generation, const SynchronizationSources &synchronizationSources,
                              const RtpClockMapping &clockMapping) {
    if (!isReady()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_.emplace(SessionState{
            .generation = generation,
            .synchronizationSources = synchronizationSources,
            .clockMapping = clockMapping,
        });
        ++stateRevision_;
        immediateReportRequested_ = false;
        nextReportTime_ = {};
    }
    condition_.notify_one();
}

void RtcpSender::recordFrame(uint64_t generation, const SentOctets &sentOctets) {
    if (!isReady()) {
        return;
    }

    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!session_.has_value() || session_->generation != generation) {
            return;
        }

        bool anyPackets = false;
        for (size_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
            if (sentOctets[channelIndex] == 0) {
                continue;
            }
            ++session_->packetCounts[channelIndex];
            session_->octetCounts[channelIndex] += sentOctets[channelIndex];
            anyPackets = true;
        }

        if (anyPackets && !session_->hasPackets) {
            session_->hasPackets = true;
            immediateReportRequested_ = true;
            notify = true;
        }
    }
    if (notify) {
        condition_.notify_one();
    }
}

void RtcpSender::endSession(uint64_t generation) {
    if (!isReady()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!session_.has_value() || session_->generation != generation) {
            return;
        }
        session_.reset();
        ++stateRevision_;
        immediateReportRequested_ = false;
        nextReportTime_ = {};
    }
    condition_.notify_one();
}

void RtcpSender::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        session_.reset();
        ++stateRevision_;
    }
    condition_.notify_one();

    if (worker_.joinable()) {
        worker_.join();
    }
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
}

std::string RtcpSender::makeCanonicalName() {
    std::array<char, 256> hostname{};
    if (gethostname(hostname.data(), hostname.size() - 1) != 0 || hostname.front() == '\0') {
        return "creature-server@unknown";
    }
    hostname.back() = '\0';
    auto canonicalName = fmt::format("creature-server@{}", hostname.data());
    canonicalName.resize(std::min<size_t>(canonicalName.size(), UINT8_MAX));
    return canonicalName;
}

bool RtcpSender::openSocket() {
    socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) {
        error("Unable to create RTCP socket: {}", std::strerror(errno));
        return false;
    }

    const int currentFlags = fcntl(socket_, F_GETFL, 0);
    if (currentFlags < 0 || fcntl(socket_, F_SETFL, currentFlags | O_NONBLOCK) < 0) {
        error("Unable to make RTCP socket non-blocking: {}", std::strerror(errno));
        close(socket_);
        socket_ = -1;
        return false;
    }

    sockaddr_in localAddress{};
    localAddress.sin_family = AF_INET;
    localAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    localAddress.sin_port = htons(RTCP_PORT);
    if (bind(socket_, reinterpret_cast<sockaddr *>(&localAddress), sizeof(localAddress)) < 0) {
        error("Unable to bind RTCP source port {}: {}", RTCP_PORT, std::strerror(errno));
        close(socket_);
        socket_ = -1;
        return false;
    }

    return true;
}

void RtcpSender::run() {
    setThreadName("RtcpSender");

    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        condition_.wait(lock, [this] { return stopping_ || (session_.has_value() && session_->hasPackets); });
        if (stopping_) {
            break;
        }

        const uint64_t revision = stateRevision_;
        const auto now = std::chrono::steady_clock::now();
        if (!immediateReportRequested_ && now < nextReportTime_) {
            condition_.wait_until(lock, nextReportTime_, [this, revision] {
                return stopping_ || stateRevision_ != revision || immediateReportRequested_;
            });
            continue;
        }

        const bool initialReport = session_->reportCount == 0;
        ++session_->reportCount;
        immediateReportRequested_ = false;
        nextReportTime_ = now + reportInterval_;
        ReportSnapshot snapshot{
            .generation = session_->generation,
            .synchronizationSources = session_->synchronizationSources,
            .clockMapping = session_->clockMapping,
            .packetCounts = session_->packetCounts,
            .octetCounts = session_->octetCounts,
            .initialReport = initialReport,
        };

        lock.unlock();
        sendReport(snapshot);
        lock.lock();
    }
}

void RtcpSender::sendReport(const ReportSnapshot &snapshot) noexcept {
    try {
        const auto timestamp = snapshot.clockMapping.at(std::chrono::steady_clock::now());
        size_t failedChannels = 0;
        uint8_t firstFailedChannel = 0;
        int firstError = 0;

        for (uint8_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
            const auto packet = buildRtcpSenderReport({
                .synchronizationSource = snapshot.synchronizationSources[channelIndex],
                .ntpTimestamp = timestamp.ntpTimestamp,
                .rtpTimestamp = timestamp.rtpTimestamp,
                .packetCount = snapshot.packetCounts[channelIndex],
                .octetCount = snapshot.octetCounts[channelIndex],
                .canonicalName = canonicalName_,
            });

            sockaddr_in destination{};
            destination.sin_family = AF_INET;
            destination.sin_port = htons(RTCP_PORT);
            if (inet_pton(AF_INET, RTP_GROUPS[channelIndex], &destination.sin_addr) != 1) {
                if (failedChannels++ == 0) {
                    firstFailedChannel = channelIndex;
                    firstError = EINVAL;
                }
                continue;
            }

            const ssize_t sent = sendto(socket_, packet.data(), packet.size(), 0,
                                        reinterpret_cast<sockaddr *>(&destination), sizeof(destination));
            if (sent < 0 || static_cast<size_t>(sent) != packet.size()) {
                if (failedChannels++ == 0) {
                    firstFailedChannel = channelIndex;
                    firstError = sent < 0 ? errno : EIO;
                }
                continue;
            }
            if (creatures::metrics) {
                creatures::metrics->incrementRtcpReportsSent();
            }
        }

        if (failedChannels > 0) {
            recordSendFailure(snapshot, timestamp, failedChannels, firstFailedChannel, firstError);
            return;
        }

        if (snapshot.initialReport) {
            info("Initial RTCP timing report sent for generation {} (SSRC {}-{}, CNAME '{}', NTP {}.{}, RTP {})",
                 snapshot.generation, snapshot.synchronizationSources.front(), snapshot.synchronizationSources.back(),
                 canonicalName_, static_cast<uint32_t>(timestamp.ntpTimestamp >> 32U),
                 static_cast<uint32_t>(timestamp.ntpTimestamp), timestamp.rtpTimestamp);
        } else {
            debug("RTCP timing report sent for generation {} at RTP timestamp {}", snapshot.generation,
                  timestamp.rtpTimestamp);
        }
    } catch (const std::exception &exception) {
        error("RTCP report generation failed for generation {}: {}", snapshot.generation, exception.what());
        if (creatures::metrics) {
            creatures::metrics->incrementRtcpSendFailures();
        }
    }
}

void RtcpSender::recordSendFailure(const ReportSnapshot &snapshot, const RtpWallClockTimestamp &timestamp,
                                   size_t failedChannels, uint8_t firstFailedChannel, int firstError) noexcept {
    try {
        if (creatures::metrics) {
            for (size_t failureIndex = 0; failureIndex < failedChannels; ++failureIndex) {
                creatures::metrics->incrementRtcpSendFailures();
            }
        }

        const auto errorMessage = fmt::format("RTCP report failed for {} channel(s); first failure channel {}: {}",
                                              failedChannels, firstFailedChannel, std::strerror(firstError));
        error("{} (generation {}, RTP timestamp {})", errorMessage, snapshot.generation, timestamp.rtpTimestamp);

        auto span = creatures::observability
                        ? creatures::observability->createOperationSpan("rtcp.sender_report.send_failure")
                        : nullptr;
        if (!span) {
            return;
        }
        span->setAttribute("rtcp.generation", static_cast<int64_t>(snapshot.generation));
        span->setAttribute("rtcp.failed_channels", static_cast<int64_t>(failedChannels));
        span->setAttribute("rtcp.first_failed_channel", static_cast<int64_t>(firstFailedChannel));
        span->setAttribute("rtcp.ssrc", static_cast<int64_t>(snapshot.synchronizationSources[firstFailedChannel]));
        span->setAttribute("rtcp.cname", canonicalName_);
        span->setAttribute("rtcp.rtp_timestamp", static_cast<int64_t>(timestamp.rtpTimestamp));
        span->setAttribute("rtcp.ntp.seconds", static_cast<int64_t>(timestamp.ntpTimestamp >> 32U));
        span->setAttribute("rtcp.ntp.fraction", static_cast<int64_t>(static_cast<uint32_t>(timestamp.ntpTimestamp)));
        span->setAttribute("error.type", "RtcpSendFailure");
        span->setAttribute("error.code", static_cast<int64_t>(firstError));
        span->setAttribute("error.message", errorMessage);
        span->setError(errorMessage);
    } catch (const std::exception &exception) {
        error("Failed to record RTCP send failure: {}", exception.what());
    } catch (...) {
        error("Failed to record RTCP send failure");
    }
}

} // namespace creatures::rtp
