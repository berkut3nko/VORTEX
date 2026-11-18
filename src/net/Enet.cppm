module;

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

export module vortex.net:enet;

import :transport;

export namespace vortex::net {

    /// @brief The port a session server listens on unless told otherwise (UDP).
    inline constexpr uint16_t kDefaultPort = 27015;

    /// @brief How an EnetTransport is set up.
    struct EnetSettings {
        /// Connections a listening transport accepts (a connecting one always has one).
        uint32_t maxPeers = 32;
        /// ENet's peer timeout: a peer that acknowledges nothing for `timeoutMinimumMs` (after
        /// ENet's resend limit) or `timeoutMaximumMs` (always) is disconnected. Longer than ENet's
        /// own 5 s minimum, so a client that loads its region for a few seconds is not dropped.
        uint32_t timeoutMinimumMs = 10000;
        uint32_t timeoutMaximumMs = 30000;
        /// Tests and drills: the fraction of incoming UDP datagrams thrown away before ENet sees
        /// them (seeded by `lossSeed`). This is real packet loss: ENet resends reliable packets
        /// itself and unreliable ones are gone. 0 in a real session.
        double receiveLoss = 0.0;
        uint64_t lossSeed = 1;
        /// The largest message this transport takes from a peer, and the bytes a peer may have
        /// waiting in it (review of stage 5: ENet reassembles a message before any token is checked,
        /// and its defaults are 32 MiB each). 0 = kListenMaxMessageBytes / kListenMaxWaitingBytes when
        /// listening (a client's largest message is under 1 KiB), ENet's 32 MiB when connecting (a
        /// client takes what the server it chose sends: one event larger than a batch goes alone).
        /// What this transport sends is not limited by it.
        size_t maxIncomingMessageBytes = 0;
        size_t maxIncomingWaitingBytes = 0;
    };

    /// @brief A listening transport's default EnetSettings::maxIncomingMessageBytes / maxIncomingWaitingBytes.
    inline constexpr size_t kListenMaxMessageBytes = 64 * 1024;
    inline constexpr size_t kListenMaxWaitingBytes = 1024 * 1024;

    /// @brief What an EnetTransport did (cumulative).
    struct EnetStats {
        uint64_t datagramsReceived = 0;   ///< UDP datagrams that reached the transport
        uint64_t datagramsDropped = 0;    ///< ... of them thrown away by EnetSettings::receiveLoss
        uint64_t messagesSent = 0, bytesSent = 0;
        uint64_t messagesReceived = 0, bytesReceived = 0;
        uint64_t sendFailures = 0;        ///< enet_peer_send refused a message (dropped, counted)
        /// Stage 8: message payload bytes by channel (State, Events, Bulk); they add up to bytesSent /
        /// bytesReceived.
        std::array<uint64_t, kChannelCount> channelBytesSent{}, channelBytesReceived{};
    };

    /**
     * @brief ITransport over UDP with ENet 1.3 (docs/design/networking.md "Transport", stage 5): one
     * ENet channel per net::Channel; the state channel is unreliable (sequenced: a datagram older than
     * one already delivered is dropped), the events and bulk channels are reliable and ordered. IPv4.
     * Main thread only; ENet runs inside Poll and Flush (no thread of its own).
     * @details ReliableBytesInFlight is exact: the bytes of reliable messages handed to ENet for that
     * peer whose packets ENet has not released yet (released = acknowledged by the peer, or dropped
     * with the connection). Disconnect is graceful (what is queued still goes out) and reports the
     * Disconnected event at once; later traffic from that peer is ignored. The destructor disconnects
     * every peer and waits up to `closeWaitMs` for the acknowledgements.
     */
    class EnetTransport final : public ITransport {
    public:
        /// @brief Listens on `bindAddress:port`; an empty address or "0.0.0.0" listens on every
        /// interface, port 0 takes any free port (Port() tells which). nullptr and `error` on failure.
        static std::unique_ptr<EnetTransport> Listen(const std::string& bindAddress, uint16_t port, EnetSettings settings, std::string* error);
        /// @brief Starts connecting to `host:port` (a name or a dotted address). Poll reports
        /// Connected, or Disconnected for the same peer when the server cannot be reached (after
        /// ENet's connect timeout). nullptr and `error` when the name does not resolve.
        static std::unique_ptr<EnetTransport> Connect(const std::string& host, uint16_t port, EnetSettings settings, std::string* error);

        ~EnetTransport() override;
        EnetTransport(const EnetTransport&) = delete;
        EnetTransport& operator=(const EnetTransport&) = delete;

        void Send(PeerId peer, Channel channel, std::span<const uint8_t> data) override;
        bool Poll(TransportEvent& out) override;
        /// The lowest of ENet's smoothed round trip, its lowest of this throttle interval and its
        /// lowest of the last one (5 s each): a sample includes the time this side did not service
        /// the socket, so a stall inflates the smoothed value for seconds, never the lowest.
        double RoundTripSeconds(PeerId peer) const override;
        size_t ReliableBytesInFlight(PeerId peer) const override;
        size_t ChannelBytesInFlight(PeerId peer, Channel channel) const override;
        void Disconnect(PeerId peer) override;
        void Flush() override;

        /// @brief ENet's smoothed round trip (what a ping display shows; stalls of this side inflate it).
        double SmoothedRoundTripSeconds(PeerId peer) const;
        /// @brief The UDP port this transport's socket is bound to.
        uint16_t Port() const;
        /// @brief Connected peers (not counting ones being disconnected).
        size_t PeerCount() const;
        /// @brief "a.b.c.d:port" of a peer, empty for an unknown one.
        std::string PeerAddress(PeerId peer) const;
        const EnetStats& Stats() const;
        /// @brief Milliseconds the destructor waits for disconnects to be acknowledged.
        void SetCloseWait(uint32_t milliseconds);

        struct Impl;

    private:
        explicit EnetTransport(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> m_Impl;
    };

    /// @brief One direction of a UdpRelay (stage 8): what the datagrams going that way meet.
    struct RelayDirection {
        double loss = 0.0;                ///< fraction dropped (seeded)
        double delaySeconds = 0.0;        ///< one-way delay
        double jitterSeconds = 0.0;       ///< uniform extra delay in [0, jitter]: datagrams may overtake each other, as with netem
        double bytesPerSecond = 0.0;      ///< a bottleneck's rate (0 = none): datagrams queue behind each other, first in first out
        /// The bottleneck's queue (bytes); a datagram that would make it longer is dropped (drop-tail, as
        /// a home router). 0 = max(16 KiB, 250 ms of the rate).
        size_t queueBytes = 0;
    };

    /// @brief How a UdpRelay is set up.
    struct RelaySettings {
        std::string serverHost;           ///< where the datagrams of the relay's client go (a name or a dotted address)
        uint16_t serverPort = kDefaultPort;
        RelayDirection up;                ///< client -> server
        RelayDirection down;              ///< server -> client
        /// Seconds after Start during which nothing gets through, either way.
        std::vector<std::pair<double, double>> outages;
        uint64_t seed = 1;
        /// Test hook (recheck of stage 8): called on the relay's thread for every datagram it admits or
        /// drops at its arrival (`direction` 0 up, 1 down; the bytes, the relay clock's time it was read,
        /// and whether it was dropped there: loss, outage, a full queue). A test judges an outage by
        /// these times, the relay's own, not by when another thread sent or read a datagram.
        std::function<void(int direction, std::span<const uint8_t> datagram, double seconds, bool dropped)> onAdmit;
    };

    /// @brief What one direction of a UdpRelay did (cumulative; datagrams and their bytes, ENet headers,
    /// acknowledgements and resends included).
    struct RelayStats {
        uint64_t datagramsIn = 0, bytesIn = 0;
        uint64_t datagramsOut = 0, bytesOut = 0;
        uint64_t lost = 0;                ///< dropped by RelayDirection::loss
        uint64_t outage = 0;              ///< dropped in an outage
        uint64_t queueFull = 0;           ///< dropped at the bottleneck's full queue
        uint64_t strangers = 0;           ///< datagrams from an address that is not this relay's client or server (dropped)
        double maxQueueSeconds = 0.0;     ///< the longest wait at the bottleneck
    };

    /**
     * @brief A UDP relay on 127.0.0.1 that impairs real traffic below ENet (docs/design/networking.md
     * "Stage 8": vortex_bot's loss, delay, jitter, outages and a slow downlink). A client's ENet connects
     * to Port(); the relay forwards each datagram to the server and back after loss, an outage, a
     * bottleneck and delay. Being below ENet, every effect is real for both ENets: resends, round trips
     * that include the queue, the server's bulk pacer seeing a slow link. Runs on a thread of its own
     * (the owner's stalls never stall the network); ENet's socket calls only, so no platform code.
     */
    class UdpRelay {
    public:
        /// @brief Binds 127.0.0.1:0 for the client and starts the thread. nullptr and `error` on failure.
        static std::unique_ptr<UdpRelay> Start(RelaySettings settings, std::string* error);
        ~UdpRelay();
        UdpRelay(const UdpRelay&) = delete;
        UdpRelay& operator=(const UdpRelay&) = delete;
        /// @brief The port on 127.0.0.1 the client connects to.
        uint16_t Port() const;
        RelayStats Up() const;            ///< client -> server
        RelayStats Down() const;          ///< server -> client
        /// @brief The relay's clock (seconds since Start; what RelaySettings::outages are measured on).
        double Seconds() const;
        /// @brief Adds an outage [from, to) on the relay's clock (review of stage 8: a test starts one
        /// once its connection is up, whenever that was).
        void AddOutage(double from, double to);

        struct Impl;

    private:
        explicit UdpRelay(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> m_Impl;
    };

    /// @brief Splits "host" or "host:port" (port 1-65535) into its parts; a missing port is
    /// `defaultPort`. false for an empty host or a malformed port.
    bool ParseEndpoint(const std::string& text, uint16_t defaultPort, std::string& host, uint16_t& port);
}
