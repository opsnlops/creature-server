#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "server/transport/ConnectionAdmission.h"

namespace creatures::transport {
namespace {

/**
 * Stands in for uWS::HttpResponse. ConnectionAdmission only ever asks a
 * response for its peer address and tells it to close, so a live socket is not
 * needed to pin the accounting down.
 */
class FakeResponse {
  public:
    explicit FakeResponse(std::string peer) : peer_(std::move(peer)) {}

    [[nodiscard]] std::string_view getRemoteAddressAsText() const { return peer_; }

    void close() { ++closeCount_; }

    [[nodiscard]] int closeCount() const { return closeCount_; }

  private:
    std::string peer_;
    int closeCount_{0};
};

using Admission = ConnectionAdmission<FakeResponse>;

/** Owns the fake responses so their addresses stay stable and distinct. */
class Peer {
  public:
    explicit Peer(std::string address) : address_(std::move(address)) { responses_.reserve(512); }

    FakeResponse &connect(Admission &admission) {
        responses_.push_back(std::make_unique<FakeResponse>(address_));
        auto &response = *responses_.back();
        lastRefusal_ = admission.update(&response, 1);
        return response;
    }

    /** The refusal the most recent connect() produced, if it was reportable. */
    [[nodiscard]] const std::optional<AdmissionRefusal> &lastRefusal() const { return lastRefusal_; }

    [[nodiscard]] const std::string &address() const { return address_; }

  private:
    std::string address_;
    std::vector<std::unique_ptr<FakeResponse>> responses_;
    std::optional<AdmissionRefusal> lastRefusal_;
};

TEST(ConnectionAdmission, AdmitsUpToThePerPeerLimitAndClosesTheRest) {
    Admission admission(100, 3);
    Peer peer("10.0.0.1");

    for (int index = 0; index < 3; ++index) {
        EXPECT_EQ(peer.connect(admission).closeCount(), 0) << "connection " << index << " should have been admitted";
    }
    EXPECT_EQ(admission.connectionsForPeer(peer.address()), 3u);

    auto &refused = peer.connect(admission);
    EXPECT_EQ(refused.closeCount(), 1) << "a connection past the per-peer limit must be closed";
    EXPECT_EQ(admission.connectionsForPeer(peer.address()), 3u);
    EXPECT_EQ(admission.refusedConnections(), 1u);
}

TEST(ConnectionAdmission, ClosingAnHttpConnectionReturnsItsSlot) {
    Admission admission(100, 1);
    Peer peer("10.0.0.1");

    auto &first = peer.connect(admission);
    EXPECT_EQ(peer.connect(admission).closeCount(), 1);

    admission.update(&first, -1);
    EXPECT_EQ(admission.connectionsForPeer(peer.address()), 0u);
    EXPECT_EQ(admission.activeConnections(), 0u);
    EXPECT_EQ(peer.connect(admission).closeCount(), 0) << "the slot should have come back";
}

TEST(ConnectionAdmission, PerPeerLimitIsNotShieldedByTheGlobalLimit) {
    Admission admission(100, 1);
    Peer first("10.0.0.1");
    Peer second("10.0.0.2");

    EXPECT_EQ(first.connect(admission).closeCount(), 0);
    EXPECT_EQ(second.connect(admission).closeCount(), 0) << "a different peer has its own budget";
    EXPECT_EQ(first.connect(admission).closeCount(), 1);
    EXPECT_EQ(admission.activeConnections(), 2u);
}

TEST(ConnectionAdmission, GlobalLimitAppliesAcrossPeers) {
    Admission admission(2, 100);
    Peer first("10.0.0.1");
    Peer second("10.0.0.2");
    Peer third("10.0.0.3");

    EXPECT_EQ(first.connect(admission).closeCount(), 0);
    EXPECT_EQ(second.connect(admission).closeCount(), 0);
    EXPECT_EQ(third.connect(admission).closeCount(), 1) << "the global limit must bind even for an unseen peer";
}

// The regression that took production down: uWS adopts an upgraded socket into
// the WebSocket context, so the HTTP close filter never fires for it again. The
// charge has to survive the upgrade and be released by the WebSocket.
TEST(ConnectionAdmission, UpgradedWebSocketReleasesItsSlotWhenItCloses) {
    Admission admission(100, 1);
    Peer peer("127.0.0.1");

    auto &upgrading = peer.connect(admission);
    auto charge = admission.transferToWebSocket(&upgrading);
    EXPECT_TRUE(charge.held);
    EXPECT_EQ(charge.peer, "127.0.0.1");

    // Still charged: an open WebSocket is a connection like any other.
    EXPECT_EQ(admission.connectionsForPeer(peer.address()), 1u);
    EXPECT_EQ(peer.connect(admission).closeCount(), 1) << "an open WebSocket must still occupy its slot";

    admission.releaseWebSocket(charge);
    EXPECT_EQ(admission.connectionsForPeer(peer.address()), 0u);
    EXPECT_EQ(admission.activeConnections(), 0u);
    EXPECT_EQ(peer.connect(admission).closeCount(), 0) << "closing the WebSocket must return its slot";
}

TEST(ConnectionAdmission, ManyWebSocketLifetimesDoNotExhaustThePeer) {
    constexpr std::size_t limit = 4;
    Admission admission(100, limit);
    Peer peer("127.0.0.1");

    // Well past the limit. Before the fix this wedged the peer permanently
    // after the fourth upgrade.
    for (std::size_t index = 0; index < limit * 10; ++index) {
        auto &response = peer.connect(admission);
        ASSERT_EQ(response.closeCount(), 0) << "upgrade " << index << " was refused";
        auto charge = admission.transferToWebSocket(&response);
        ASSERT_TRUE(charge.held) << "upgrade " << index << " carried no charge";
        admission.releaseWebSocket(charge);
    }

    EXPECT_EQ(admission.activeConnections(), 0u);
    EXPECT_EQ(admission.connectionsForPeer(peer.address()), 0u);
    EXPECT_EQ(admission.refusedConnections(), 0u);
}

// The counter was only half the leak: the pointer-keyed map had no one to erase
// an upgraded socket's entry either.
TEST(ConnectionAdmission, TransferStopsTrackingTheDeadResponsePointer) {
    Admission admission(100, 10);
    Peer peer("127.0.0.1");

    auto &upgrading = peer.connect(admission);
    EXPECT_EQ(admission.trackedConnections(), 1u);

    auto charge = admission.transferToWebSocket(&upgrading);
    EXPECT_EQ(admission.trackedConnections(), 0u) << "the dangling response pointer must not be kept";

    admission.releaseWebSocket(charge);
    EXPECT_EQ(admission.trackedConnections(), 0u);
}

// After the transfer the response pointer is dead, but uWS has been known to
// surprise us. A stray close for a transferred socket must not double-release.
TEST(ConnectionAdmission, StrayCloseAfterTransferDoesNotDoubleRelease) {
    Admission admission(100, 2);
    Peer peer("127.0.0.1");

    auto &upgrading = peer.connect(admission);
    auto charge = admission.transferToWebSocket(&upgrading);
    admission.update(&upgrading, -1);

    EXPECT_EQ(admission.connectionsForPeer(peer.address()), 1u) << "the WebSocket still holds the charge";
    admission.releaseWebSocket(charge);
    EXPECT_EQ(admission.connectionsForPeer(peer.address()), 0u);
}

TEST(ConnectionAdmission, ReleasingTheSameChargeTwiceIsHarmless) {
    Admission admission(100, 2);
    Peer peer("127.0.0.1");

    auto &first = peer.connect(admission);
    peer.connect(admission);
    auto charge = admission.transferToWebSocket(&first);

    admission.releaseWebSocket(charge);
    EXPECT_EQ(admission.activeConnections(), 1u);
    admission.releaseWebSocket(charge);
    EXPECT_EQ(admission.activeConnections(), 1u) << "a second release must not credit a slot that was never taken";
}

TEST(ConnectionAdmission, RefusedConnectionCarriesNoChargeToUpgrade) {
    Admission admission(100, 1);
    Peer peer("127.0.0.1");

    peer.connect(admission);
    auto &refused = peer.connect(admission);
    ASSERT_EQ(refused.closeCount(), 1);

    auto charge = admission.transferToWebSocket(&refused);
    EXPECT_FALSE(charge.held) << "a refused connection never took a slot, so it has none to hand over";

    admission.releaseWebSocket(charge);
    EXPECT_EQ(admission.connectionsForPeer(peer.address()), 1u) << "releasing it must not credit a phantom slot";
}

TEST(ConnectionAdmission, TransferOfAnUnknownResponseIsEmpty) {
    Admission admission(100, 1);
    FakeResponse stranger("10.0.0.9");

    const auto charge = admission.transferToWebSocket(&stranger);
    EXPECT_FALSE(charge.held);
    EXPECT_TRUE(charge.peer.empty());
    EXPECT_EQ(admission.activeConnections(), 0u);
}

TEST(ConnectionAdmission, ShutdownRefusesNewConnections) {
    Admission admission(100, 100);
    Peer peer("10.0.0.1");

    EXPECT_EQ(peer.connect(admission).closeCount(), 0);
    admission.beginShutdown();
    EXPECT_EQ(peer.connect(admission).closeCount(), 1);
    EXPECT_EQ(admission.activeConnections(), 1u);
}

TEST(ConnectionAdmission, PeerWithNoAddressIsAccountedSeparately) {
    Admission admission(100, 1);
    Peer anonymous("");
    Peer named("10.0.0.1");

    EXPECT_EQ(anonymous.connect(admission).closeCount(), 0);
    EXPECT_EQ(admission.connectionsForPeer("unknown"), 1u);
    EXPECT_EQ(named.connect(admission).closeCount(), 0) << "a real address must not share the unknown bucket";
    EXPECT_EQ(anonymous.connect(admission).closeCount(), 1);
}

// The refusal payload is what reaches Honeycomb, so its contents matter as
// much as the accounting. admission.scope / limit / in_flight are the
// vocabulary StreamingAdHocSession and JobWorker already use.
TEST(ConnectionAdmission, PeerLimitRefusalDescribesThePeerBudget) {
    Admission admission(100, 2);
    Peer peer("127.0.0.1");

    peer.connect(admission);
    peer.connect(admission);
    peer.connect(admission);

    const auto &refusal = peer.lastRefusal();
    ASSERT_TRUE(refusal.has_value());
    EXPECT_EQ(refusal->peer, "127.0.0.1");
    EXPECT_EQ(refusal->scope, ADMISSION_SCOPE_PEER);
    EXPECT_EQ(refusal->limit, 2u) << "the per-peer limit is the one that bound";
    EXPECT_EQ(refusal->inFlight, 2u);
    EXPECT_EQ(refusal->refusalsFromPeer, 1u);
}

TEST(ConnectionAdmission, GlobalLimitRefusalDescribesTheGlobalBudget) {
    Admission admission(2, 100);
    Peer first("10.0.0.1");
    Peer second("10.0.0.2");
    Peer third("10.0.0.3");

    first.connect(admission);
    second.connect(admission);
    third.connect(admission);

    const auto &refusal = third.lastRefusal();
    ASSERT_TRUE(refusal.has_value());
    EXPECT_EQ(refusal->scope, ADMISSION_SCOPE_GLOBAL);
    EXPECT_EQ(refusal->limit, 2u) << "the global limit is the one that bound";
    EXPECT_EQ(refusal->inFlight, 2u);
}

TEST(ConnectionAdmission, ShutdownRefusalSaysSo) {
    Admission admission(100, 100);
    Peer peer("10.0.0.1");

    admission.beginShutdown();
    peer.connect(admission);

    const auto &refusal = peer.lastRefusal();
    ASSERT_TRUE(refusal.has_value());
    EXPECT_EQ(refusal->scope, ADMISSION_SCOPE_SHUTDOWN);
}

// A saturated peer retrying forever must not be able to flood the log or the
// trace, but the counter still has to be exact.
TEST(ConnectionAdmission, RefusalReportingIsThrottledButCountingIsNot) {
    Admission admission(1000, 1);
    Peer peer("127.0.0.1");
    peer.connect(admission);

    std::size_t reported = 0;
    for (std::size_t attempt = 1; attempt <= ADMISSION_REFUSAL_REPORT_INTERVAL * 2; ++attempt) {
        peer.connect(admission);
        if (peer.lastRefusal().has_value()) {
            ++reported;
        }
    }

    EXPECT_EQ(admission.refusedConnections(), ADMISSION_REFUSAL_REPORT_INTERVAL * 2) << "every refusal must be counted";
    EXPECT_EQ(reported, 3u) << "the first refusal, plus one per report interval";
}

TEST(ConnectionAdmission, BeingAdmittedAgainLetsThePeerAnnounceItsNextEpisode) {
    Admission admission(1000, 1);
    Peer peer("127.0.0.1");

    auto &held = peer.connect(admission);
    peer.connect(admission);
    ASSERT_TRUE(peer.lastRefusal().has_value()) << "first refusal of the episode";
    peer.connect(admission);
    ASSERT_FALSE(peer.lastRefusal().has_value()) << "throttled while still saturated";

    // Recover, then saturate again: the new episode gets its own first refusal.
    admission.update(&held, -1);
    auto &reconnected = peer.connect(admission);
    ASSERT_FALSE(peer.lastRefusal().has_value()) << "this one was admitted";
    peer.connect(admission);
    EXPECT_TRUE(peer.lastRefusal().has_value()) << "a fresh episode must announce itself";
    admission.update(&reconnected, -1);
}

} // namespace
} // namespace creatures::transport
