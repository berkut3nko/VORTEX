module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

module vortex.net;

import vortex.core;

namespace vortex::net {

    // --- LoopbackTransport ------------------------------------------------------------------------

    struct LoopbackTransport::Inbox {
        std::deque<TransportEvent> events;
        size_t reliableBytes = 0;     // undelivered reliable bytes in this inbox
        std::array<size_t, kChannelCount> channelBytes{};   // ... per channel
    };

    LoopbackTransport::LoopbackTransport() : m_Inbox(std::make_shared<Inbox>()) {}

    LoopbackTransport::~LoopbackTransport() {
        std::vector<PeerId> peers;
        for (const auto& [peer, link] : m_Links) peers.push_back(peer);
        for (const PeerId peer : peers) Disconnect(peer);
    }

    PeerId LoopbackTransport::Connect(LoopbackTransport& a, LoopbackTransport& b) {
        const PeerId idOfB = a.m_NextPeer++;
        const PeerId idOfA = b.m_NextPeer++;
        a.m_Links[idOfB] = Link{b.m_Inbox, idOfA};
        b.m_Links[idOfA] = Link{a.m_Inbox, idOfB};
        a.m_Inbox->events.push_back({TransportEvent::Type::Connected, idOfB, Channel::State, {}});
        b.m_Inbox->events.push_back({TransportEvent::Type::Connected, idOfA, Channel::State, {}});
        return idOfB;
    }

    void LoopbackTransport::Send(PeerId peer, Channel channel, std::span<const uint8_t> data) {
        const auto it = m_Links.find(peer);
        if (it == m_Links.end()) return;
        const std::shared_ptr<Inbox> remote = it->second.remote.lock();
        if (!remote) return;
        remote->events.push_back({TransportEvent::Type::Received, it->second.remoteIdForUs, channel, std::vector<uint8_t>(data.begin(), data.end())});
        if (IsReliable(channel)) {
            remote->reliableBytes += data.size();
            remote->channelBytes[static_cast<size_t>(channel)] += data.size();
        }
    }

    bool LoopbackTransport::Poll(TransportEvent& out) {
        if (m_Inbox->events.empty()) return false;
        out = std::move(m_Inbox->events.front());
        m_Inbox->events.pop_front();
        if (out.type == TransportEvent::Type::Received && IsReliable(out.channel)) {
            m_Inbox->reliableBytes -= std::min(m_Inbox->reliableBytes, out.data.size());
            size_t& channel = m_Inbox->channelBytes[static_cast<size_t>(out.channel)];
            channel -= std::min(channel, out.data.size());
        }
        if (out.type == TransportEvent::Type::Disconnected) m_Links.erase(out.peer);
        return true;
    }

    double LoopbackTransport::RoundTripSeconds(PeerId) const { return 0.0; }

    size_t LoopbackTransport::ReliableBytesInFlight(PeerId peer) const {
        const auto it = m_Links.find(peer);
        if (it == m_Links.end()) return 0;
        const std::shared_ptr<Inbox> remote = it->second.remote.lock();
        return remote ? remote->reliableBytes : 0;   // upper bound: everything queued at the other side
    }

    size_t LoopbackTransport::ChannelBytesInFlight(PeerId peer, Channel channel) const {
        const auto it = m_Links.find(peer);
        if (it == m_Links.end() || !IsReliable(channel)) return 0;
        const std::shared_ptr<Inbox> remote = it->second.remote.lock();
        return remote ? remote->channelBytes[static_cast<size_t>(channel)] : 0;
    }

    void LoopbackTransport::Disconnect(PeerId peer) {
        const auto it = m_Links.find(peer);
        if (it == m_Links.end()) return;
        if (const std::shared_ptr<Inbox> remote = it->second.remote.lock())
            remote->events.push_back({TransportEvent::Type::Disconnected, it->second.remoteIdForUs, Channel::State, {}});
        m_Inbox->events.push_back({TransportEvent::Type::Disconnected, peer, Channel::State, {}});
        m_Links.erase(it);
    }

    // --- ImpairedTransport ------------------------------------------------------------------------

    ImpairedTransport::ImpairedTransport(ITransport& inner, Impairment impairment, uint64_t seed, std::function<double()> clock)
        : m_Inner(inner), m_Impairment(std::move(impairment)), m_Rng(seed), m_Clock(std::move(clock)), m_Seed(seed) {}

    double ImpairedTransport::Uniform() { return static_cast<double>((m_Active ? *m_Active : m_Rng).NextU32()) * (1.0 / 4294967296.0); }

    bool ImpairedTransport::InOutage(double t, double& end) const {
        for (const Impairment::Outage& o : m_Impairment.outages) {
            if (t >= o.start && t < o.end) {
                end = o.end;
                return true;
            }
        }
        return false;
    }

    void ImpairedTransport::Send(PeerId peer, Channel channel, std::span<const uint8_t> data) {
        const double now = m_Clock();
        const Impairment& im = m_Impairment;
        m_Active = im.rngPerPeer ? &m_PeerRng.try_emplace(peer, m_Seed ^ (static_cast<uint64_t>(peer) * 0x9E3779B97F4A7C15ull), peer).first->second
                                 : &m_Rng;
        ++m_Stats.sent;
        m_Stats.bytes += data.size();
        // Link rate: a packet leaves once the ones before it have.
        double leave = now;
        if (im.bytesPerSecond > 0.0) {
            leave = std::max(now, m_LinkFree) + static_cast<double>(data.size()) / im.bytesPerSecond;
            m_LinkFree = leave;
        }
        if (im.peerBytesPerSecond > 0.0) {
            double& free = m_PeerLinkFree[peer];
            leave = std::max(leave, free) + static_cast<double>(data.size()) / im.peerBytesPerSecond;
            free = leave;
        }
        double due = leave + im.delaySeconds + im.jitterSeconds * Uniform();
        const bool reliable = IsReliable(channel);
        double outageEnd = 0.0;
        if (!reliable) {
            if (Uniform() < im.loss || InOutage(now, outageEnd)) {
                ++m_Stats.lost;
                return;
            }
            if (Uniform() < im.reorder) {
                due += im.delaySeconds + im.jitterSeconds + 0.001;
                ++m_Stats.reordered;
            }
        } else {
            const double resend = im.resendSeconds > 0.0 ? im.resendSeconds : 2.0 * im.delaySeconds + im.jitterSeconds + 0.02;
            // Every lost attempt costs one resend; an attempt during an outage is lost too.
            double attempt = now;
            for (int tries = 0; tries < 64; ++tries) {
                const bool outage = InOutage(attempt, outageEnd);
                if (!outage && !(Uniform() < im.loss)) break;
                attempt = outage ? std::max(attempt + resend, outageEnd) : attempt + resend;
                due = std::max(due, attempt + im.delaySeconds + im.jitterSeconds * Uniform());
                ++m_Stats.resent;
            }
            // Ordered: never before an earlier packet of the same peer and channel.
            const uint64_t key = (static_cast<uint64_t>(peer) << 8) | static_cast<uint64_t>(channel);
            double& last = m_LastDue[key];
            due = std::max(due, last);
            last = due;
        }
        auto insert = [this](Packet packet) {
            const auto at = std::upper_bound(m_Queue.begin(), m_Queue.end(), packet,
                                             [](const Packet& a, const Packet& b) { return a.due < b.due || (a.due == b.due && a.order < b.order); });
            m_Queue.insert(at, std::move(packet));
        };
        Packet packet{due, m_Order++, peer, channel, std::vector<uint8_t>(data.begin(), data.end())};
        if (!reliable && Uniform() < im.duplicate) {
            Packet copy = packet;
            copy.order = m_Order++;
            copy.due += im.jitterSeconds * Uniform();
            insert(std::move(copy));
            ++m_Stats.duplicated;
        }
        insert(std::move(packet));
        Release();
    }

    void ImpairedTransport::Flush() {
        Release();
        m_Inner.Flush();
    }

    void ImpairedTransport::Release() {
        const double now = m_Clock();
        while (!m_Queue.empty() && m_Queue.front().due <= now) {
            Packet packet = std::move(m_Queue.front());
            m_Queue.pop_front();
            m_Inner.Send(packet.peer, packet.channel, packet.data);
        }
    }

    bool ImpairedTransport::Poll(TransportEvent& out) {
        Release();
        return m_Inner.Poll(out);
    }

    double ImpairedTransport::RoundTripSeconds(PeerId peer) const {
        // What waits in this side's link queues delays the round trip too (as ENet's RTT grows with
        // its queue: plan amendment 1).
        double queued = 0.0;
        if (m_Impairment.bytesPerSecond > 0.0 || m_Impairment.peerBytesPerSecond > 0.0) {
            const double now = m_Clock();
            double free = m_LinkFree;
            if (const auto it = m_PeerLinkFree.find(peer); it != m_PeerLinkFree.end()) free = std::max(free, it->second);
            queued = std::max(0.0, free - now);
        }
        return m_Inner.RoundTripSeconds(peer) + 2.0 * m_Impairment.delaySeconds + m_Impairment.jitterSeconds + queued;
    }

    size_t ImpairedTransport::ReliableBytesInFlight(PeerId peer) const {
        size_t bytes = m_Inner.ReliableBytesInFlight(peer);
        for (const Packet& p : m_Queue)
            if (p.peer == peer && IsReliable(p.channel)) bytes += p.data.size();
        return bytes;
    }

    size_t ImpairedTransport::ChannelBytesInFlight(PeerId peer, Channel channel) const {
        size_t bytes = m_Inner.ChannelBytesInFlight(peer, channel);
        if (!IsReliable(channel)) return bytes;
        for (const Packet& p : m_Queue)
            if (p.peer == peer && p.channel == channel) bytes += p.data.size();
        return bytes;
    }

    void ImpairedTransport::Disconnect(PeerId peer) {
        std::erase_if(m_Queue, [peer](const Packet& p) { return p.peer == peer; });
        m_Inner.Disconnect(peer);
    }
}
