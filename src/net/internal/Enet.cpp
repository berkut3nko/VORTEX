module;

// Windows (docs/two-pc-test-windows.md 4.9): <enet/enet.h> includes <winsock2.h> / <windows.h>, whose
// min / max macros break std::min / std::max below and whose full set of headers is not needed.
// Nothing on other platforms.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <enet/enet.h>
#if defined(_WIN32)
#include <mswsock.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <functional>
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

        Endpoint ToEndpoint(const ENetAddress& a) { return {ENET_NET_TO_HOST_32(a.host), a.port}; }
        ENetAddress ToEnet(Endpoint e) {
            ENetAddress a{};
            a.host = ENET_HOST_TO_NET_32(e.ip);
            a.port = e.port;
            return a;
        }

        /// rendezvous.md "The engine side": on Windows one ICMP "port unreachable" (a dead LAN candidate
        /// punched) would make the socket's next receive fail with WSAECONNRESET and end ENet's receive
        /// loop early; SIO_UDP_CONNRESET off stops that. Linux reports nothing on an unconnected socket.
        void IgnoreConnectionResets([[maybe_unused]] ENetSocket socket) {
#if defined(_WIN32)
            BOOL report = FALSE;
            DWORD returned = 0;
            WSAIoctl(socket, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#endif
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

        /// Called by ENet for every datagram before it parses it: the receive-loss emulation, then STUN
        /// answers and punch datagrams (TakeRaw), then a client host's filter.
        static int ENET_CALLBACK Intercept(ENetHost* host, ENetEvent*);

        /// rendezvous.md: raw datagrams the intercept took, the STUN requests this host sent, and a
        /// client host's server (Open: none until ConnectTo).
        std::deque<Datagram> raw;
        std::array<StunTransaction, 16> stunSent{};
        size_t stunSentCount = 0;
        bool clientHost = false;
        std::optional<ENetAddress> server;

        bool SentStun(const StunTransaction& t) const {
            for (size_t i = 0; i < std::min(stunSentCount, stunSent.size()); ++i)
                if (stunSent[i] == t) return true;
            return false;
        }

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
                        stats.channelBytesReceived[e.channelID] += e.packet->dataLength;
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
        /// Every transport's host (ENet's host has no user pointer): the intercept finds its transport
        /// here. Transports may live on different threads (tests, bots), so the map has a lock.
        std::mutex g_HostsMutex;
        std::unordered_map<ENetHost*, EnetTransport::Impl*> g_Hosts;

        EnetTransport::Impl* FindHost(ENetHost* host) {
            std::lock_guard lock(g_HostsMutex);
            const auto it = g_Hosts.find(host);
            return it != g_Hosts.end() ? it->second : nullptr;
        }
    }

    int ENET_CALLBACK EnetTransport::Impl::Intercept(ENetHost* host, ENetEvent*) {
        Impl* found = FindHost(host);
        if (found == nullptr) return 0;
        Impl& s = *found;
        if (s.settings.receiveLoss > 0.0) {
            const double u = static_cast<double>(s.rng.NextU32()) * (1.0 / 4294967296.0);
            if (u < s.settings.receiveLoss) {
                ++s.stats.datagramsDropped;
                return 1;   // consumed: ENet never sees it (it resends what was reliable)
            }
        }
        const std::span<const uint8_t> bytes(host->receivedData, host->receivedDataLength);
        if (s.settings.interceptRaw) {
            StunTransaction t{};
            // A punch datagram, or an answer to a STUN request this host sent: never ENet's.
            if (HasPunchMagic(bytes) || (LooksLikeStun(bytes, &t) && s.SentStun(t))) {
                if (s.raw.size() >= kMaxRawWaiting) {
                    ++s.stats.rawDropped;
                } else {
                    s.raw.push_back({ToEndpoint(host->receivedAddress), std::vector<uint8_t>(bytes.begin(), bytes.end())});
                    ++s.stats.rawTaken;
                }
                return 1;
            }
        }
        // A client host takes ENet traffic from its server only (none before ConnectTo).
        if (s.clientHost && (!s.server || s.server->host != host->receivedAddress.host || s.server->port != host->receivedAddress.port)) {
            ++s.stats.strangersDropped;
            return 1;
        }
        return 0;
    }

    namespace {
        /// Makes the host of a transport: SIO_UDP_CONNRESET off, the intercept always installed.
        ENetHost* CreateHost(EnetTransport::Impl& impl, const ENetAddress* address, size_t peers) {
            ENetHost* host = enet_host_create(address, peers, kChannelCount, 0, 0);
            if (host == nullptr) return nullptr;
            IgnoreConnectionResets(host->socket);
            host->intercept = &EnetTransport::Impl::Intercept;
            std::lock_guard lock(g_HostsMutex);
            g_Hosts[host] = &impl;
            return host;
        }

        void DestroyHost(ENetHost* host) {
            {
                std::lock_guard lock(g_HostsMutex);
                g_Hosts.erase(host);
            }
            enet_host_destroy(host);
        }
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
        impl->host = CreateHost(*impl, &address, std::max<uint32_t>(1, settings.maxPeers));
        if (impl->host == nullptr) {
            ReleaseEnet();
            if (error) *error = "could not listen on UDP port " + std::to_string(port) + " (in use, or not allowed)";
            return nullptr;
        }
        // A stranger's messages are reassembled by ENet before any token is checked: a listening
        // transport takes only what a client sends (under 1 KiB) with room to spare.
        impl->LimitIncoming(settings.maxIncomingMessageBytes != 0 ? settings.maxIncomingMessageBytes : kListenMaxMessageBytes,
                            settings.maxIncomingWaitingBytes != 0 ? settings.maxIncomingWaitingBytes : kListenMaxWaitingBytes);
        return std::unique_ptr<EnetTransport>(new EnetTransport(std::move(impl)));
    }

    std::unique_ptr<EnetTransport> EnetTransport::Connect(const std::string& host, uint16_t port, EnetSettings settings, std::string* error) {
        Endpoint server;
        if (!ResolveEndpoint(host, port, server, error)) return nullptr;
        server.port = port;
        std::unique_ptr<EnetTransport> transport = Open({}, 0, settings, error);
        if (!transport || !transport->ConnectTo(server, error)) return nullptr;
        return transport;
    }

    std::unique_ptr<EnetTransport> EnetTransport::Open(const std::string& bindAddress, uint16_t port, EnetSettings settings, std::string* error) {
        if (!AcquireEnet()) {
            if (error) *error = "ENet could not be initialised";
            return nullptr;
        }
        ENetAddress address{};
        address.host = ENET_HOST_ANY;
        address.port = port;
        if (!bindAddress.empty() && bindAddress != "0.0.0.0" && enet_address_set_host_ip(&address, bindAddress.c_str()) != 0) {
            ReleaseEnet();
            if (error) *error = "'" + bindAddress + "' is not an IPv4 address to bind";
            return nullptr;
        }
        auto impl = std::make_unique<Impl>(settings);
        impl->clientHost = true;
        // Bound now (not at the first send): STUN and lan candidates need the port before anything goes out.
        impl->host = CreateHost(*impl, &address, 1);
        if (impl->host == nullptr) {
            ReleaseEnet();
            if (error) *error = "could not open a UDP socket" + (port != 0 ? " on port " + std::to_string(port) : std::string());
            return nullptr;
        }
        // A client takes what the server it chose sends (ENet's defaults unless set).
        impl->LimitIncoming(settings.maxIncomingMessageBytes, settings.maxIncomingWaitingBytes);
        return std::unique_ptr<EnetTransport>(new EnetTransport(std::move(impl)));
    }

    bool EnetTransport::ConnectTo(Endpoint server, std::string* error) {
        Impl& s = *m_Impl;
        if (!s.clientHost || s.server) {
            if (error) *error = "this transport cannot start a connection (not made by Open, or connected already)";
            return false;
        }
        const ENetAddress address = ToEnet(server);
        ENetPeer* peer = enet_host_connect(s.host, &address, kChannelCount, 0);
        if (peer == nullptr) {
            if (error) *error = "could not start a connection";
            return false;
        }
        s.server = address;
        PeerInfo& info = s.Adopt(peer);
        info.outgoing = true;
        info.connectStartMs = enet_time_get();
        return true;
    }

    void EnetTransport::SendRaw(Endpoint to, std::span<const uint8_t> bytes) {
        Impl& s = *m_Impl;
        StunTransaction t{};
        if (ParseStunBindingRequest(bytes, t)) {
            s.stunSent[s.stunSentCount % s.stunSent.size()] = t;
            ++s.stunSentCount;
        }
        const ENetAddress address = ToEnet(to);
        ENetBuffer buffer;
        buffer.data = const_cast<uint8_t*>(bytes.data());
        buffer.dataLength = bytes.size();
        if (enet_socket_send(s.host->socket, &address, &buffer, 1) < 0) ++s.stats.sendFailures;
    }

    std::vector<Datagram> EnetTransport::TakeRaw() {
        Impl& s = *m_Impl;
        std::vector<Datagram> out(std::make_move_iterator(s.raw.begin()), std::make_move_iterator(s.raw.end()));
        s.raw.clear();
        return out;
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
        // Destroying the host resets the remaining peers and releases their packets: the records
        // they point to must still exist until then.
        DestroyHost(s.host);
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
        s.stats.channelBytesSent[static_cast<size_t>(channel)] += data.size();
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

    // --- UdpRelay (stage 8) ----------------------------------------------------------------------------

    struct UdpRelay::Impl {
        RelaySettings settings;
        ENetSocket down = ENET_SOCKET_NULL;   // bound to 127.0.0.1: the client's side
        ENetSocket up = ENET_SOCKET_NULL;     // talks to the server
        ENetAddress server{};
        ENetAddress client{};
        bool haveClient = false;
        uint16_t port = 0;
        std::atomic<bool> stop{false};
        std::thread thread;
        mutable std::mutex statsMutex;
        RelayStats stats[2];                  // [0] up, [1] down
        bool acquired = false;

        struct Datagram {
            double due = 0.0;
            uint64_t order = 0;
            std::vector<uint8_t> bytes;
            bool operator>(const Datagram& o) const { return due != o.due ? due > o.due : order > o.order; }
        };
        struct Direction {
            RelayDirection settings;
            Rng rng{1};
            double lastDeparture = 0.0;
            std::priority_queue<Datagram, std::vector<Datagram>, std::greater<Datagram>> queue;
        };
        Direction dirs[2];
        uint64_t order = 0;
        std::chrono::steady_clock::time_point start;

        double Now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
        double Uniform(Direction& d) { return static_cast<double>(d.rng.NextU32()) * (1.0 / 4294967296.0); }

        bool InOutage(double t) const {
            for (const auto& [a, b] : settings.outages)
                if (t >= a && t < b) return true;
            return false;
        }

        /// A datagram arrives at the relay going way `k`: dropped, or queued for its due time.
        void Admit(int k, const uint8_t* data, size_t size, double now) {
            Direction& d = dirs[k];
            std::lock_guard lock(statsMutex);
            RelayStats& st = stats[k];
            ++st.datagramsIn;
            st.bytesIn += size;
            const auto seen = [&](bool dropped) {
                if (settings.onAdmit) settings.onAdmit(k, std::span<const uint8_t>(data, size), now, dropped);
            };
            if (InOutage(now)) {
                ++st.outage;
                seen(true);
                return;
            }
            if (d.settings.loss > 0.0 && Uniform(d) < d.settings.loss) {
                ++st.lost;
                seen(true);
                return;
            }
            double departure = now;
            if (d.settings.bytesPerSecond > 0.0) {
                const double rate = d.settings.bytesPerSecond;
                const double cap = d.settings.queueBytes != 0 ? static_cast<double>(d.settings.queueBytes) : std::max(16384.0, 0.25 * rate);
                departure = std::max(now, d.lastDeparture) + static_cast<double>(size) / rate;
                if ((departure - now) * rate > cap + static_cast<double>(size)) {
                    ++st.queueFull;
                    seen(true);
                    return;
                }
                d.lastDeparture = departure;
                st.maxQueueSeconds = std::max(st.maxQueueSeconds, departure - now);
            }
            Datagram g;
            g.due = departure + d.settings.delaySeconds + (d.settings.jitterSeconds > 0.0 ? d.settings.jitterSeconds * Uniform(d) : 0.0);
            g.order = order++;
            g.bytes.assign(data, data + size);
            d.queue.push(std::move(g));
            seen(false);
        }

        void Release(int k, double now) {
            Direction& d = dirs[k];
            while (!d.queue.empty() && d.queue.top().due <= now) {
                const Datagram& g = d.queue.top();
                const ENetSocket to = k == 0 ? up : down;
                const ENetAddress& where = k == 0 ? server : client;
                if (k == 0 || haveClient) {
                    ENetBuffer buffer;
                    buffer.data = const_cast<uint8_t*>(g.bytes.data());
                    buffer.dataLength = g.bytes.size();
                    if (enet_socket_send(to, &where, &buffer, 1) > 0) {
                        std::lock_guard lock(statsMutex);
                        ++stats[k].datagramsOut;
                        stats[k].bytesOut += g.bytes.size();
                    }
                }
                d.queue.pop();
            }
        }

        void Drain(ENetSocket socket, bool fromClient, std::vector<uint8_t>& buffer, double now) {
            for (;;) {
                ENetAddress from{};
                ENetBuffer b;
                b.data = buffer.data();
                b.dataLength = buffer.size();
                const int n = enet_socket_receive(socket, &from, &b, 1);
                if (n <= 0) return;   // nothing waiting (0) or an error (an ICMP report on Linux): next round
                if (fromClient) {
                    if (!haveClient) {
                        client = from;
                        haveClient = true;
                    } else if (from.host != client.host || from.port != client.port) {
                        std::lock_guard lock(statsMutex);
                        ++stats[0].strangers;
                        continue;
                    }
                    Admit(0, buffer.data(), static_cast<size_t>(n), now);
                } else {
                    if (from.host != server.host || from.port != server.port) {
                        std::lock_guard lock(statsMutex);
                        ++stats[1].strangers;
                        continue;
                    }
                    Admit(1, buffer.data(), static_cast<size_t>(n), now);
                }
            }
        }

        void Run() {
            std::vector<uint8_t> buffer(65536);
            while (!stop.load(std::memory_order_relaxed)) {
                const double now = Now();
                Release(0, now);
                Release(1, now);
                ENetSocketSet set;
                ENET_SOCKETSET_EMPTY(set);
                ENET_SOCKETSET_ADD(set, down);
                ENET_SOCKETSET_ADD(set, up);
                // Wake for a datagram or within 1 ms (the next due one is at most that late).
                const int ready = enet_socketset_select(std::max(down, up), &set, nullptr, 1);
                if (ready <= 0) continue;
                const double t = Now();
                if (ENET_SOCKETSET_CHECK(set, down)) Drain(down, true, buffer, t);
                if (ENET_SOCKETSET_CHECK(set, up)) Drain(up, false, buffer, t);
            }
        }

        ~Impl() {
            stop = true;
            if (thread.joinable()) thread.join();
            if (down != ENET_SOCKET_NULL) enet_socket_destroy(down);
            if (up != ENET_SOCKET_NULL) enet_socket_destroy(up);
            if (acquired) ReleaseEnet();
        }
    };

    UdpRelay::UdpRelay(std::unique_ptr<Impl> impl) : m_Impl(std::move(impl)) {}
    UdpRelay::~UdpRelay() = default;

    std::unique_ptr<UdpRelay> UdpRelay::Start(RelaySettings settings, std::string* error) {
        auto impl = std::make_unique<Impl>();
        impl->settings = settings;
        if (!AcquireEnet()) {
            if (error) *error = "ENet could not be initialised";
            return nullptr;
        }
        impl->acquired = true;
        if (enet_address_set_host(&impl->server, settings.serverHost.c_str()) != 0) {
            if (error) *error = "'" + settings.serverHost + "' does not resolve to an IPv4 address";
            return nullptr;
        }
        impl->server.port = settings.serverPort;
        impl->down = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        impl->up = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if (impl->down == ENET_SOCKET_NULL || impl->up == ENET_SOCKET_NULL) {
            if (error) *error = "could not open the relay's UDP sockets";
            return nullptr;
        }
        ENetAddress local{};
        enet_address_set_host_ip(&local, "127.0.0.1");
        local.port = 0;
        ENetAddress any{};
        any.host = ENET_HOST_ANY;
        any.port = 0;
        if (enet_socket_bind(impl->down, &local) != 0 || enet_socket_bind(impl->up, &any) != 0) {
            if (error) *error = "could not bind the relay's UDP sockets";
            return nullptr;
        }
        enet_socket_set_option(impl->down, ENET_SOCKOPT_NONBLOCK, 1);
        enet_socket_set_option(impl->up, ENET_SOCKOPT_NONBLOCK, 1);
        // Room for a bottleneck's bursts in the kernel too (ENet's own host asks for 256 KiB).
        enet_socket_set_option(impl->down, ENET_SOCKOPT_RCVBUF, 1 << 20);
        enet_socket_set_option(impl->up, ENET_SOCKOPT_RCVBUF, 1 << 20);
        ENetAddress bound{};
        if (enet_socket_get_address(impl->down, &bound) != 0) {
            if (error) *error = "could not read the relay's port";
            return nullptr;
        }
        impl->port = bound.port;
        impl->dirs[0].settings = settings.up;
        impl->dirs[1].settings = settings.down;
        impl->dirs[0].rng = Rng(settings.seed * 2 + 1);
        impl->dirs[1].rng = Rng(settings.seed * 2 + 2);
        impl->start = std::chrono::steady_clock::now();
        Impl* raw = impl.get();
        impl->thread = std::thread([raw] { raw->Run(); });
        return std::unique_ptr<UdpRelay>(new UdpRelay(std::move(impl)));
    }

    uint16_t UdpRelay::Port() const { return m_Impl->port; }
    RelayStats UdpRelay::Up() const {
        std::lock_guard lock(m_Impl->statsMutex);
        return m_Impl->stats[0];
    }
    RelayStats UdpRelay::Down() const {
        std::lock_guard lock(m_Impl->statsMutex);
        return m_Impl->stats[1];
    }
    double UdpRelay::Seconds() const { return m_Impl->Now(); }
    void UdpRelay::AddOutage(double from, double to) {
        std::lock_guard lock(m_Impl->statsMutex);   // Admit reads the outages under it
        m_Impl->settings.outages.emplace_back(from, to);
    }

    bool ResolveEndpoint(const std::string& text, uint16_t defaultPort, Endpoint& out, std::string* error) {
        std::string host;
        uint16_t port = defaultPort;
        if (!ParseEndpoint(text, defaultPort, host, port)) {
            if (error) *error = "'" + text + "' is not HOST or HOST:PORT";
            return false;
        }
        if (!AcquireEnet()) {
            if (error) *error = "ENet could not be initialised";
            return false;
        }
        ENetAddress address{};
        const bool ok = enet_address_set_host(&address, host.c_str()) == 0;
        ReleaseEnet();
        if (!ok) {
            if (error) *error = "'" + host + "' does not resolve to an IPv4 address";
            return false;
        }
        out = {ENET_NET_TO_HOST_32(address.host), port};
        return true;
    }

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
