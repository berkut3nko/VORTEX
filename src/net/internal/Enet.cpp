module;

#include <enet/enet.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

module vortex.net;

import vortex.core;

namespace vortex::net {

    namespace {
        /// enet_initialize / enet_deinitialize are process-wide: counted over every transport.
        int g_EnetUsers = 0;

        bool AcquireEnet() {
            if (g_EnetUsers == 0 && enet_initialize() != 0) return false;
            ++g_EnetUsers;
            return true;
        }

        void ReleaseEnet() {
            if (g_EnetUsers > 0 && --g_EnetUsers == 0) enet_deinitialize();
        }

        /// One ENet peer as the transport knows it. Lives until ENet has reset the peer (its
        /// Disconnect event, or the host's destruction): the packets queued for it point here.
        struct PeerInfo;
        /// A reliable packet's owner: its peer and channel (ReleasePacket gives the bytes back to both).
        struct ChannelBytes {
            PeerInfo* info = nullptr;
            size_t bytes = 0;
        };
        struct PeerInfo {
            PeerInfo() {
                for (ChannelBytes& c : channels) c.info = this;
            }
            PeerInfo(const PeerInfo&) = delete;
            PeerInfo& operator=(const PeerInfo&) = delete;
            ENetPeer* peer = nullptr;
            PeerId id = 0;
            size_t reliableBytes = 0;     ///< reliable bytes handed to ENet and not released yet
            std::array<ChannelBytes, kChannelCount> channels{};   ///< ... per channel (review of stage 7a)
            bool connected = false;       ///< ENet reported the connection
            bool live = true;             ///< false once Disconnect was called: nothing more is reported
            bool outgoing = false;        ///< this side connected (EnetTransport::Connect)
            enet_uint32 connectStartMs = 0;   ///< enet_time_get() when the connect was started
        };

        /// ENet releases a packet when every peer it was queued for has acknowledged it (reliable)
        /// or when the connection is reset: the bytes are no longer in flight.
        void ENET_CALLBACK ReleasePacket(ENetPacket* packet) {
            if (auto* channel = static_cast<ChannelBytes*>(packet->userData)) {
                channel->bytes -= std::min(channel->bytes, packet->dataLength);
                channel->info->reliableBytes -= std::min(channel->info->reliableBytes, packet->dataLength);
            }
        }

        std::string FormatAddress(const ENetAddress& a) {
            char text[64] = {};
            if (enet_address_get_host_ip(&a, text, sizeof(text)) != 0) return {};
            return std::string(text) + ":" + std::to_string(a.port);
        }
    }

    struct EnetTransport::Impl {
        ENetHost* host = nullptr;
        EnetSettings settings;
        Rng rng;
        std::unordered_map<PeerId, std::unique_ptr<PeerInfo>> peers;   // every peer ENet has not reset yet
        std::deque<TransportEvent> events;
        PeerId next = 1;
        EnetStats stats;
        uint32_t closeWaitMs = 500;

        explicit Impl(EnetSettings s) : settings(s), rng(s.lossSeed) {}

        PeerInfo* Live(PeerId id) const {
            const auto it = peers.find(id);
            return it != peers.end() && it->second->live ? it->second.get() : nullptr;
        }

        PeerInfo& Adopt(ENetPeer* peer) {
            auto info = std::make_unique<PeerInfo>();
            info->peer = peer;
            info->id = next++;
            PeerInfo& ref = *info;
            peer->data = info.get();
            peers.emplace(ref.id, std::move(info));
            enet_peer_timeout(peer, 0, settings.timeoutMinimumMs, settings.timeoutMaximumMs);
            return ref;
        }

        /// The receive-loss emulation: called by ENet for every datagram before it parses it.
        static int ENET_CALLBACK Intercept(ENetHost* host, ENetEvent*);

        void Handle(const ENetEvent& e) {
            auto* info = static_cast<PeerInfo*>(e.peer != nullptr ? e.peer->data : nullptr);
            switch (e.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    if (info == nullptr) info = &Adopt(e.peer);   // an incoming connection
                    info->connected = true;
                    if (info->outgoing) SeedRoundTrip(*e.peer, *info);
                    if (info->live) events.push_back({TransportEvent::Type::Connected, info->id, Channel::State, {}});
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    if (info != nullptr && info->live && e.channelID < kChannelCount) {
                        const auto* bytes = static_cast<const uint8_t*>(e.packet->data);
                        ++stats.messagesReceived;
                        stats.bytesReceived += e.packet->dataLength;
                        events.push_back({TransportEvent::Type::Received, info->id, static_cast<Channel>(e.channelID),
                                          std::vector<uint8_t>(bytes, bytes + e.packet->dataLength)});
                    }
                    enet_packet_destroy(e.packet);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT: {
                    // ENet has reset the peer: its queued packets are released, the record can go.
                    if (info != nullptr) {
                        if (info->live) events.push_back({TransportEvent::Type::Disconnected, info->id, Channel::State, {}});
                        e.peer->data = nullptr;
                        peers.erase(info->id);
                    }
                    break;
                }
                default: break;
            }
        }

        /// ENet starts its smoothed round trip at 500 ms and moves it 1/8 per acknowledged reliable
        /// message; a client sends few of them (Hello, then ENet's pings every 500 ms), so it still
        /// says about 440 ms when the prediction starts and needs seconds to come down - and the
        /// client sizes its lead from it (review of stage 5: an over-large lead held the clock in a
        /// hard resync a second later). The connect handshake is a round trip of its own (plus at
        /// most one service interval of this side): it seeds the estimate, as ENet means its first
        /// sample to.
        static void SeedRoundTrip(ENetPeer& peer, const PeerInfo& info) {
            const enet_uint32 rtt = std::max<enet_uint32>(1, enet_time_get() - info.connectStartMs);
            peer.roundTripTime = rtt;
            peer.roundTripTimeVariance = (rtt + 1) / 2;
            peer.lowestRoundTripTime = rtt;
            peer.lastRoundTripTime = rtt;
        }

        void Service() {
            ENetEvent e;
            while (enet_host_service(host, &e, 0) > 0) Handle(e);
        }

        /// What ENet takes from a peer (review of stage 5): ENet's host limit applies to receiving
        /// (checked before a fragmented message is allocated) and to enet_peer_send, so Send lifts it
        /// for its own messages; `maxPacket` is what the other side may send.
        void LimitIncoming(size_t maxPacket, size_t maxWaiting) {
            receiveLimit = maxPacket != 0 ? maxPacket : host->maximumPacketSize;
            host->maximumPacketSize = receiveLimit;
            if (maxWaiting != 0) host->maximumWaitingData = maxWaiting;
        }

        size_t receiveLimit = 0;
    };

    namespace {
        /// Hosts whose transport emulates loss (ENet's host has no user pointer). Main thread only.
        std::unordered_map<ENetHost*, EnetTransport::Impl*> g_LossyHosts;
    }

    int ENET_CALLBACK EnetTransport::Impl::Intercept(ENetHost* host, ENetEvent*) {
        const auto it = g_LossyHosts.find(host);
        if (it == g_LossyHosts.end()) return 0;
        Impl& s = *it->second;
        const double u = static_cast<double>(s.rng.NextU32()) * (1.0 / 4294967296.0);
        if (u < s.settings.receiveLoss) {
            ++s.stats.datagramsDropped;
            return 1;   // consumed: ENet never sees it (it resends what was reliable)
        }
        return 0;
    }

    EnetTransport::EnetTransport(std::unique_ptr<Impl> impl) : m_Impl(std::move(impl)) {}

    std::unique_ptr<EnetTransport> EnetTransport::Listen(const std::string& bindAddress, uint16_t port, EnetSettings settings, std::string* error) {
        if (!AcquireEnet()) {
            if (error) *error = "ENet could not be initialised";
            return nullptr;
        }
        ENetAddress address{};
        address.host = ENET_HOST_ANY;
        address.port = port;
        if (!bindAddress.empty() && bindAddress != "0.0.0.0" && enet_address_set_host_ip(&address, bindAddress.c_str()) != 0) {
            ReleaseEnet();
            if (error) *error = "'" + bindAddress + "' is not an IPv4 address to listen on";
            return nullptr;
        }
        auto impl = std::make_unique<Impl>(settings);
        impl->host = enet_host_create(&address, std::max<uint32_t>(1, settings.maxPeers), kChannelCount, 0, 0);
        if (impl->host == nullptr) {
            ReleaseEnet();
            if (error) *error = "could not listen on UDP port " + std::to_string(port) + " (in use, or not allowed)";
            return nullptr;
        }
        if (settings.receiveLoss > 0.0) {
            impl->host->intercept = &Impl::Intercept;
            g_LossyHosts[impl->host] = impl.get();
        }
        // A stranger's messages are reassembled by ENet before any token is checked: a listening
        // transport takes only what a client sends (under 1 KiB) with room to spare.
        impl->LimitIncoming(settings.maxIncomingMessageBytes != 0 ? settings.maxIncomingMessageBytes : kListenMaxMessageBytes,
                            settings.maxIncomingWaitingBytes != 0 ? settings.maxIncomingWaitingBytes : kListenMaxWaitingBytes);
        return std::unique_ptr<EnetTransport>(new EnetTransport(std::move(impl)));
    }

    std::unique_ptr<EnetTransport> EnetTransport::Connect(const std::string& host, uint16_t port, EnetSettings settings, std::string* error) {
        if (!AcquireEnet()) {
            if (error) *error = "ENet could not be initialised";
            return nullptr;
        }
        ENetAddress address{};
        if (enet_address_set_host(&address, host.c_str()) != 0) {
            ReleaseEnet();
            if (error) *error = "'" + host + "' does not resolve to an IPv4 address";
            return nullptr;
        }
        address.port = port;
        auto impl = std::make_unique<Impl>(settings);
        impl->host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
        if (impl->host == nullptr) {
            ReleaseEnet();
            if (error) *error = "could not open a UDP socket";
            return nullptr;
        }
        if (settings.receiveLoss > 0.0) {
            impl->host->intercept = &Impl::Intercept;
            g_LossyHosts[impl->host] = impl.get();
        }
        // A client takes what the server it chose sends (ENet's defaults unless set).
        impl->LimitIncoming(settings.maxIncomingMessageBytes, settings.maxIncomingWaitingBytes);
        ENetPeer* peer = enet_host_connect(impl->host, &address, kChannelCount, 0);
        if (peer == nullptr) {
            g_LossyHosts.erase(impl->host);
            enet_host_destroy(impl->host);
            ReleaseEnet();
            if (error) *error = "could not start a connection";
            return nullptr;
        }
        PeerInfo& info = impl->Adopt(peer);
        info.outgoing = true;
        info.connectStartMs = enet_time_get();
        return std::unique_ptr<EnetTransport>(new EnetTransport(std::move(impl)));
    }

    EnetTransport::~EnetTransport() {
        Impl& s = *m_Impl;
        // Graceful close: every peer is told, and the acknowledgements are waited for a moment so
        // the other side sees a disconnect now rather than a timeout later.
        std::vector<PeerId> gone;
        for (auto& [id, info] : s.peers) {
            if (info->live) enet_peer_disconnect(info->peer, 0);
            info->live = false;
            // A connection that was never made is reset by ENet at once (no event follows).
            if (info->peer->state == ENET_PEER_STATE_DISCONNECTED) {
                info->peer->data = nullptr;
                gone.push_back(id);
            }
        }
        for (const PeerId id : gone) s.peers.erase(id);
        if (!s.peers.empty()) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(s.closeWaitMs);
            ENetEvent e;
            while (!s.peers.empty() && std::chrono::steady_clock::now() < until) {
                if (enet_host_service(s.host, &e, 10) > 0) s.Handle(e);
            }
        }
        g_LossyHosts.erase(s.host);
        // Destroying the host resets the remaining peers and releases their packets: the records
        // they point to must still exist until then.
        enet_host_destroy(s.host);
        s.peers.clear();
        ReleaseEnet();
    }

    void EnetTransport::Send(PeerId peer, Channel channel, std::span<const uint8_t> data) {
        Impl& s = *m_Impl;
        PeerInfo* info = s.Live(peer);
        if (info == nullptr || !info->connected) return;
        const bool reliable = IsReliable(channel);
        const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
        ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
        if (packet == nullptr) {
            ++s.stats.sendFailures;
            return;
        }
        if (reliable) {
            ChannelBytes& counted = info->channels[static_cast<size_t>(channel)];
            packet->userData = &counted;
            packet->freeCallback = &ReleasePacket;
            info->reliableBytes += data.size();
            counted.bytes += data.size();
        }
        // The host's packet limit is meant for what peers send here (LimitIncoming); this side's own
        // messages may be larger (a large event goes alone), so it is lifted for this call only.
        s.host->maximumPacketSize = std::max(s.receiveLimit, data.size());
        const int sent = enet_peer_send(info->peer, static_cast<enet_uint8>(channel), packet);
        s.host->maximumPacketSize = s.receiveLimit;
        if (sent != 0) {
            enet_packet_destroy(packet);   // not taken: released here (and uncounted by the callback)
            ++s.stats.sendFailures;
            return;
        }
        ++s.stats.messagesSent;
        s.stats.bytesSent += data.size();
    }

    bool EnetTransport::Poll(TransportEvent& out) {
        Impl& s = *m_Impl;
        if (s.events.empty()) s.Service();
        if (s.events.empty()) return false;
        out = std::move(s.events.front());
        s.events.pop_front();
        return true;
    }

    double EnetTransport::RoundTripSeconds(PeerId peer) const {
        const PeerInfo* info = m_Impl->Live(peer);
        if (info == nullptr) return 0.0;
        const ENetPeer& p = *info->peer;
        return static_cast<double>(std::min({p.roundTripTime, p.lowestRoundTripTime, p.lastRoundTripTime})) / 1000.0;
    }

    double EnetTransport::SmoothedRoundTripSeconds(PeerId peer) const {
        const PeerInfo* info = m_Impl->Live(peer);
        return info != nullptr ? static_cast<double>(info->peer->roundTripTime) / 1000.0 : 0.0;
    }

    size_t EnetTransport::ReliableBytesInFlight(PeerId peer) const {
        const PeerInfo* info = m_Impl->Live(peer);
        return info != nullptr ? info->reliableBytes : 0;
    }

    size_t EnetTransport::ChannelBytesInFlight(PeerId peer, Channel channel) const {
        const PeerInfo* info = m_Impl->Live(peer);
        return info != nullptr && IsReliable(channel) ? info->channels[static_cast<size_t>(channel)].bytes : 0;
    }

    void EnetTransport::Disconnect(PeerId peer) {
        Impl& s = *m_Impl;
        PeerInfo* info = s.Live(peer);
        if (info == nullptr) return;
        info->live = false;
        // Queued messages (a refusal, the last events) still go out first; ENet then closes it.
        if (info->connected) {
            enet_peer_disconnect_later(info->peer, 0);
        } else {
            // Never connected: nothing to tell the other side.
            info->peer->data = nullptr;
            enet_peer_reset(info->peer);
            s.peers.erase(peer);
        }
        s.events.push_back({TransportEvent::Type::Disconnected, peer, Channel::State, {}});
    }

    void EnetTransport::Flush() { enet_host_flush(m_Impl->host); }

    uint16_t EnetTransport::Port() const {
        ENetAddress address{};
        if (enet_socket_get_address(m_Impl->host->socket, &address) != 0) return 0;
        return address.port;
    }

    size_t EnetTransport::PeerCount() const {
        size_t n = 0;
        for (const auto& [id, info] : m_Impl->peers) n += info->live && info->connected ? 1u : 0u;
        return n;
    }

    std::string EnetTransport::PeerAddress(PeerId peer) const {
        const PeerInfo* info = m_Impl->Live(peer);
        return info != nullptr ? FormatAddress(info->peer->address) : std::string();
    }

    const EnetStats& EnetTransport::Stats() const {
        m_Impl->stats.datagramsReceived = m_Impl->host->totalReceivedPackets;   // ENet counts them (dropped ones too)
        return m_Impl->stats;
    }
    void EnetTransport::SetCloseWait(uint32_t milliseconds) { m_Impl->closeWaitMs = milliseconds; }

    bool ParseEndpoint(const std::string& text, uint16_t defaultPort, std::string& host, uint16_t& port) {
        const size_t colon = text.rfind(':');
        host = colon == std::string::npos ? text : text.substr(0, colon);
        port = defaultPort;
        if (host.empty()) return false;
        if (colon == std::string::npos) return true;
        const std::string digits = text.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        const long value = std::strtol(digits.c_str(), nullptr, 10);
        if (value < 1 || value > 65535) return false;
        port = static_cast<uint16_t>(value);
        return true;
    }
}
