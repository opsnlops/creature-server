#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace creatures::transport {

/**
 * A connection's charge against the admission limits.
 *
 * Normally a charge lives and dies with the HTTP socket that opened it, and
 * ConnectionAdmission releases it when uWebSockets reports the socket closed.
 * A socket that upgrades to a WebSocket is the exception: it leaves the HTTP
 * context entirely, so the charge has to travel with the WebSocket session and
 * be released from the WebSocket close handler instead.
 */
struct AdmissionCharge {
    std::string peer;
    bool held{false};
};

/** Which budget a connection was refused against, in admission.scope terms. */
constexpr std::string_view ADMISSION_SCOPE_PEER = "connection.peer";
constexpr std::string_view ADMISSION_SCOPE_GLOBAL = "connection.global";
constexpr std::string_view ADMISSION_SCOPE_SHUTDOWN = "server.shutdown";

/**
 * Once a peer is at its limit, the first refusal is worth reporting and so is
 * every Nth after it. The counters still count every single one; this only
 * bounds how many log lines and spans a saturated peer can produce.
 */
constexpr std::size_t ADMISSION_REFUSAL_REPORT_INTERVAL = 100;

/**
 * A refusal worth telling someone about.
 *
 * A refused connection is closed before its request is read, so it never
 * becomes a request, a span or a status code on its own. Reporting one is the
 * caller's job, because only the caller knows about logging and tracing — this
 * struct is everything they need to describe it.
 */
struct AdmissionRefusal {
    std::string peer;
    std::string_view scope;
    std::size_t limit{0};
    std::size_t inFlight{0};
    std::size_t refusalsFromPeer{0};
};

/**
 * Global and per-peer admission control for inbound connections.
 *
 * uWebSockets reports socket lifetime through its HTTP-context filter: +1 when
 * a socket opens, -1 when it closes. That accounting has exactly one hole in
 * it. When a request upgrades to a WebSocket, uWS adopts the socket into the
 * WebSocket context (`us_socket_context_adopt_socket`), which unlinks it from
 * the HTTP context — so the HTTP context's close callback never fires for that
 * socket again, and a -1 never arrives.
 *
 * An upgraded connection therefore has to hand its charge to the WebSocket
 * session, which releases it from the WebSocket close handler. Without that
 * handoff every WebSocket ever opened becomes a permanent phantom charged to
 * its peer. Once a peer reaches its limit, every later connection from it is
 * closed before its request is even read, which reaches a reverse proxy as a
 * connection reset and reaches the client as the proxy's own 500. Because
 * Apache proxies to localhost, every external client shares the loopback
 * peer's budget, so the whole API goes dark while the server itself stays
 * perfectly healthy. That was issue #216.
 *
 * Pure accounting on purpose: no logging, no tracing, no globals. Templated on
 * the response type so it can be exercised without a live socket. A Response
 * needs only `getRemoteAddressAsText()` and `close()`.
 *
 * Not thread safe, and does not need to be: every caller runs on the
 * uWebSockets loop thread.
 */
template <typename Response> class ConnectionAdmission {
  public:
    ConnectionAdmission(const std::size_t maximumConnections, const std::size_t maximumConnectionsPerPeer)
        : maximumConnections_(maximumConnections), maximumConnectionsPerPeer_(maximumConnectionsPerPeer) {}

    /**
     * uWebSockets' HTTP filter: change is +1 when a socket opens, -1 when it
     * closes. Returns a refusal when this call turned a connection away and
     * that refusal is one worth reporting; refusedConnections() still counts
     * every refusal, reported or not.
     */
    std::optional<AdmissionRefusal> update(Response *response, const int change) {
        if (change > 0) {
            return opened(response);
        }
        if (change < 0) {
            closed(response);
        }
        return std::nullopt;
    }

    /**
     * Hand this connection's charge to the WebSocket that is about to adopt its
     * socket. Must be called while the response is still alive, immediately
     * before upgrading, because upgrading invalidates it.
     *
     * The charge keeps counting against the limits for as long as the WebSocket
     * is open — a WebSocket is a connection like any other — so the returned
     * charge has to reach releaseWebSocket() when it closes.
     */
    [[nodiscard]] AdmissionCharge transferToWebSocket(Response *response) {
        const auto connection = connections_.find(static_cast<const void *>(response));
        if (connection == connections_.end()) {
            return {};
        }
        AdmissionCharge charge{.peer = connection->second.peer, .held = connection->second.admitted};
        // Drop the pointer entry: the HTTP close filter will never fire for
        // this socket, so nothing would ever erase it, and the response pointer
        // is about to dangle. The counts stay charged; the charge owns them now.
        connections_.erase(connection);
        return charge;
    }

    /** Release a charge handed out by transferToWebSocket(). Safe to call more than once. */
    void releaseWebSocket(AdmissionCharge &charge) {
        if (!charge.held) {
            return;
        }
        charge.held = false;
        release(charge.peer);
    }

    void beginShutdown() { accepting_ = false; }

    /** Connections currently charged, including upgraded WebSockets. */
    [[nodiscard]] std::size_t activeConnections() const { return activeConnections_; }

    [[nodiscard]] std::size_t connectionsForPeer(const std::string &peer) const {
        const auto found = connectionsPerPeer_.find(peer);
        return found == connectionsPerPeer_.end() ? std::size_t{0} : found->second;
    }

    /**
     * Connections still tracked by response pointer. Upgraded sockets are not
     * among them, which is what keeps the map from growing without bound.
     */
    [[nodiscard]] std::size_t trackedConnections() const { return connections_.size(); }

    /** Every connection ever refused, whether or not its refusal was reported. */
    [[nodiscard]] std::size_t refusedConnections() const { return refusedConnections_; }

  private:
    struct Connection {
        std::string peer;
        bool admitted{false};
    };

    std::optional<AdmissionRefusal> opened(Response *response) {
        std::string peer(response->getRemoteAddressAsText());
        if (peer.empty()) {
            peer = "unknown";
        }

        const auto peerConnections = connectionsForPeer(peer);
        const bool atPeerLimit = peerConnections >= maximumConnectionsPerPeer_;
        const bool atGlobalLimit = activeConnections_ >= maximumConnections_;
        const bool admitted = accepting_ && !atGlobalLimit && !atPeerLimit;
        // Record before closing: close() can re-enter closed() synchronously,
        // and it has to find the entry to know this connection was never
        // charged.
        connections_.insert_or_assign(static_cast<const void *>(response),
                                      Connection{.peer = peer, .admitted = admitted});
        if (!admitted) {
            auto refusal = refuse(peer, atGlobalLimit, peerConnections);
            response->close();
            return refusal;
        }

        ++activeConnections_;
        connectionsPerPeer_[peer] = peerConnections + 1;
        // This peer is healthy again, so the next saturation episode gets to
        // announce itself instead of being swallowed by an old refusal count.
        if (!refusalsPerPeer_.empty()) {
            refusalsPerPeer_.erase(peer);
        }
        return std::nullopt;
    }

    void closed(Response *response) {
        const auto connection = connections_.find(static_cast<const void *>(response));
        if (connection == connections_.end()) {
            return;
        }
        if (connection->second.admitted) {
            release(connection->second.peer);
        }
        connections_.erase(connection);
    }

    void release(const std::string &peer) {
        if (activeConnections_ > 0) {
            --activeConnections_;
        }
        const auto found = connectionsPerPeer_.find(peer);
        if (found == connectionsPerPeer_.end()) {
            return;
        }
        if (found->second > 1) {
            --found->second;
        } else {
            connectionsPerPeer_.erase(found);
        }
    }

    std::optional<AdmissionRefusal> refuse(const std::string &peer, const bool atGlobalLimit,
                                           const std::size_t peerConnections) {
        ++refusedConnections_;
        // Unlike the connection maps, this one is not bounded by the limits: a
        // peer that is refused once and never returns leaves an entry behind.
        // Refusals are exactly what a flood of distinct addresses produces, so
        // forget everything rather than grow without bound. The only cost is
        // that a peer may announce its first refusal more than once.
        if (refusalsPerPeer_.size() > maximumConnections_) {
            refusalsPerPeer_.clear();
        }
        const auto refusals = ++refusalsPerPeer_[peer];
        if (refusals != 1 && refusals % ADMISSION_REFUSAL_REPORT_INTERVAL != 0) {
            return std::nullopt;
        }

        const auto scope = !accepting_     ? ADMISSION_SCOPE_SHUTDOWN
                           : atGlobalLimit ? ADMISSION_SCOPE_GLOBAL
                                           : ADMISSION_SCOPE_PEER;
        return AdmissionRefusal{.peer = peer,
                                .scope = scope,
                                .limit =
                                    scope == ADMISSION_SCOPE_GLOBAL ? maximumConnections_ : maximumConnectionsPerPeer_,
                                .inFlight = scope == ADMISSION_SCOPE_GLOBAL ? activeConnections_ : peerConnections,
                                .refusalsFromPeer = refusals};
    }

    std::size_t maximumConnections_;
    std::size_t maximumConnectionsPerPeer_;
    std::size_t activeConnections_{0};
    std::size_t refusedConnections_{0};
    bool accepting_{true};
    std::unordered_map<const void *, Connection> connections_;
    std::unordered_map<std::string, std::size_t> connectionsPerPeer_;
    std::unordered_map<std::string, std::size_t> refusalsPerPeer_;
};

} // namespace creatures::transport
