module;

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

export module vortex.net:transport;

import vortex.core;

export namespace vortex::net {

    /// @brief Identifies the other end of a connection, as one transport sees it.
    using PeerId = uint32_t;

    /// @brief The three channels of docs/design/networking.md "Transport". Values are wire format.
    enum class Channel : uint8_t {
        State = 0,    ///< unreliable: inputs up, snapshots / status down
        Events = 1,   ///< reliable, ordered: handshake, the opSeq stream
        Bulk = 2,     ///< reliable, ordered, paced by the application: chunk data (stage 7)
    };
    inline constexpr uint32_t kChannelCount = 3;
    constexpr bool IsReliable(Channel channel) { return channel != Channel::State; }

    struct TransportEvent {
        enum class Type : uint8_t { Connected, Received, Disconnected };
        Type type = Type::Received;
        PeerId peer = 0;
        Channel channel = Channel::State;
        std::vector<uint8_t> data;
    };

    /**
     * @brief A message transport with ENet's semantics: per peer three channels; reliable channels
     * deliver every message exactly once and in order, the state channel may lose, duplicate or
     * reorder. Messages are whole byte strings (no stream framing). Main thread only.
     * @details Implemented by LoopbackTransport (in process), ImpairedTransport (a decorator that
     * delays, loses, duplicates, reorders, rate-limits and cuts packets) and EnetTransport (UDP,
     * networking stage 5). Sends may wait inside the transport until Poll or Flush (ENet sends its
     * queued datagrams then): an owner that sends during a tick flushes at its end.
     */
    class ITransport {
    public:
        virtual ~ITransport() = default;
        /// @brief Queues `data` for `peer` on `channel`. Unknown peers are ignored.
        virtual void Send(PeerId peer, Channel channel, std::span<const uint8_t> data) = 0;
        /// @brief Next event, false when there is none now.
        virtual bool Poll(TransportEvent& out) = 0;
        /// @brief Round-trip estimate to `peer` in seconds (0 for a loopback): a recent one that a
        /// stall of this side (a long frame, a blocking load) does not inflate - the client sizes
        /// its lead from it (review of stage 5; ENet: the lowest smoothed round trip of the last
        /// 5-10 s, EnetTransport::SmoothedRoundTripSeconds is ENet's own smoothed value).
        virtual double RoundTripSeconds(PeerId peer) const = 0;
        /// @brief Bytes on reliable channels sent to `peer` and not yet delivered.
        virtual size_t ReliableBytesInFlight(PeerId peer) const = 0;
        /// @brief Closes the connection; both sides get a Disconnected event (this side at once).
        virtual void Disconnect(PeerId peer) = 0;
        /// @brief Hands whatever Send queued to the network now (ENet: one datagram per peer instead
        /// of waiting for the next Poll). In-process transports deliver at once and need nothing.
        virtual void Flush() {}
    };

    /**
     * @brief An in-process transport. Two endpoints are linked with Connect; each then sees a
     * Connected event for the other. Delivery is immediate and lossless (wrap it in an
     * ImpairedTransport to see a network). Destroying an endpoint disconnects its peers.
     */
    class LoopbackTransport final : public ITransport {
    public:
        LoopbackTransport();
        ~LoopbackTransport() override;
        LoopbackTransport(const LoopbackTransport&) = delete;
        LoopbackTransport& operator=(const LoopbackTransport&) = delete;

        /// @brief Links two endpoints. Returns the id `a` uses for `b` (and `b` gets its own id for `a`).
        static PeerId Connect(LoopbackTransport& a, LoopbackTransport& b);

        void Send(PeerId peer, Channel channel, std::span<const uint8_t> data) override;
        bool Poll(TransportEvent& out) override;
        double RoundTripSeconds(PeerId peer) const override;
        size_t ReliableBytesInFlight(PeerId peer) const override;
        void Disconnect(PeerId peer) override;

    private:
        struct Inbox;
        struct Link {
            std::weak_ptr<Inbox> remote;
            PeerId remoteIdForUs = 0;   ///< the id the other side uses for us
        };
        std::shared_ptr<Inbox> m_Inbox;
        std::unordered_map<PeerId, Link> m_Links;
        PeerId m_NextPeer = 1;
    };

    /// @brief What an ImpairedTransport does to the packets it sends (est. values in the tests).
    struct Impairment {
        double loss = 0.0;               ///< Probability a packet is lost. Reliable channels resend it.
        double delaySeconds = 0.0;       ///< One-way delay.
        double jitterSeconds = 0.0;      ///< Uniform extra delay in [0, jitter].
        double duplicate = 0.0;          ///< State channel: probability a packet arrives twice.
        double reorder = 0.0;            ///< State channel: probability a packet is held back one extra delay.
        double bytesPerSecond = 0.0;     ///< Link rate (0 = unlimited): packets queue behind each other.
        /// Reliable resend after a loss (ENet resends after about one round trip); 0 = 2 x delay +
        /// jitter + 20 ms.
        double resendSeconds = 0.0;
        struct Outage {
            double start = 0.0, end = 0.0;   ///< Seconds on the transport's clock: nothing gets through.
        };
        std::vector<Outage> outages;
    };

    /// @brief What an ImpairedTransport did (cumulative).
    struct ImpairmentStats {
        uint64_t sent = 0;
        uint64_t lost = 0;               ///< State channel packets dropped (loss or outage).
        uint64_t resent = 0;             ///< Reliable packets delayed by a simulated resend.
        uint64_t duplicated = 0;
        uint64_t reordered = 0;
        uint64_t bytes = 0;
    };

    /**
     * @brief A decorator that makes a transport behave like a bad network, on the sending side,
     * from a seeded random sequence (no root, no real network; docs/design/networking.md).
     * @details Packets wait in a queue until their delivery time on `clock` and are handed to the
     * inner transport by Poll. ENet semantics are kept: on the state channel a lost packet is gone,
     * a duplicate arrives twice and a reordered one late; on a reliable channel a loss costs one
     * resend delay per lost attempt and order is kept (a packet never overtakes an earlier one of
     * its channel and peer: head-of-line blocking), so "5 % loss" shows as latency. Outages drop
     * state packets and hold reliable ones until a resend after the outage. Poll every tick.
     */
    class ImpairedTransport final : public ITransport {
    public:
        ImpairedTransport(ITransport& inner, Impairment impairment, uint64_t seed, std::function<double()> clock);

        void Send(PeerId peer, Channel channel, std::span<const uint8_t> data) override;
        bool Poll(TransportEvent& out) override;
        double RoundTripSeconds(PeerId peer) const override;
        size_t ReliableBytesInFlight(PeerId peer) const override;
        void Disconnect(PeerId peer) override;
        /// @brief Hands the packets that are due to the inner transport and flushes it.
        void Flush() override;

        const ImpairmentStats& Stats() const { return m_Stats; }
        Impairment& Settings() { return m_Impairment; }
        /// @brief Packets still queued.
        size_t Queued() const { return m_Queue.size(); }

    private:
        struct Packet {
            double due = 0.0;
            uint64_t order = 0;
            PeerId peer = 0;
            Channel channel = Channel::State;
            std::vector<uint8_t> data;
        };
        void Release();
        double Uniform();
        bool InOutage(double t, double& end) const;

        ITransport& m_Inner;
        Impairment m_Impairment;
        Rng m_Rng;
        std::function<double()> m_Clock;
        std::deque<Packet> m_Queue;      // sorted by (due, order)
        std::unordered_map<uint64_t, double> m_LastDue;   // (peer, channel) -> latest reliable due time
        double m_LinkFree = 0.0;
        uint64_t m_Order = 0;
        ImpairmentStats m_Stats;
    };
}
