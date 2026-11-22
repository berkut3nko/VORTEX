// net::NatSim (docs/design/rendezvous.md "The engine side", tests only): NATs in memory, so hole
// punching is tested over every row of "Who can meet" without a network. A NAT has a mapping rule
// (endpoint-independent, or symmetric: a new outside port per destination), a filtering rule
// (endpoint-independent, address-dependent, port-dependent), a pool of outside IPs (symmetric pooling
// takes the next one per mapping), hairpinning, a mapping timeout (refreshed by outbound traffic only)
// and can block outbound UDP. Sockets live inside a NAT (a private address) or on the internet
// (public). Datagrams take `latency` across the internet and `lanLatency` inside one NAT's LAN; the
// filter is applied when a datagram reaches the NAT (simultaneous open works as on a real one).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

import vortex.net;

namespace natsim {

    using vortex::net::Datagram;
    using vortex::net::Endpoint;

    enum class Mapping { Independent, Symmetric };
    enum class Filtering { Independent, Address, Port };

    struct NatSettings {
        Mapping mapping = Mapping::Independent;
        Filtering filtering = Filtering::Port;
        std::vector<uint32_t> outsideIps;   ///< the pool (at least one)
        bool hairpin = false;
        double mappingTimeout = 120.0;
        bool blockOutbound = false;
    };

    class NatSim {
    public:
        double latency = 0.02;      ///< one way across the internet (s)
        double lanLatency = 0.001;  ///< one way inside a LAN

        int AddNat(NatSettings s) {
            m_Nats.push_back({std::move(s), {}, 40000, 0});
            return static_cast<int>(m_Nats.size()) - 1;
        }

        /// A socket at `at`: inside NAT `nat` (a private address), or public with nat -1.
        void AddSocket(int nat, Endpoint at) { m_Sockets[at] = nat; }

        void Send(Endpoint from, Endpoint to, std::vector<uint8_t> bytes, double now) {
            const auto self = m_Sockets.find(from);
            if (self == m_Sockets.end()) return;
            const int nat = self->second;
            if (nat < 0) {   // a public socket
                m_Flight.push_back({now + latency, from, to, std::move(bytes), -1});
                return;
            }
            // Inside a NAT: a LAN neighbour is reached directly.
            const auto peer = m_Sockets.find(to);
            if (peer != m_Sockets.end() && peer->second == nat) {
                m_Flight.push_back({now + lanLatency, from, to, std::move(bytes), -2});
                return;
            }
            Nat& n = m_Nats[static_cast<size_t>(nat)];
            if (n.s.blockOutbound || vortex::net::IsPrivateIpv4(to.ip)) return;   // blocked, or not routable
            MapEntry& m = MapFor(n, from, to, now);
            m.lastOut = now;
            m.sentTo.insert(to);
            m_Flight.push_back({now + latency, m.outside, to, std::move(bytes), nat});
        }

        /// Moves every datagram due by `now` to its socket's inbox, in time order.
        void Step(double now) {
            std::stable_sort(m_Flight.begin(), m_Flight.end(), [](const Flight& a, const Flight& b) { return a.due < b.due; });
            size_t done = 0;
            for (; done < m_Flight.size() && m_Flight[done].due <= now; ++done) Arrive(m_Flight[done]);
            m_Flight.erase(m_Flight.begin(), m_Flight.begin() + static_cast<std::ptrdiff_t>(done));
        }

        /// What reached `socket` (the source as the socket sees it).
        std::vector<Datagram> Receive(Endpoint socket) {
            std::vector<Datagram> out;
            out.swap(m_Inbox[socket]);
            return out;
        }

        uint64_t Delivered() const { return m_Delivered; }

    private:
        struct MapEntry {
            Endpoint inside, destination, outside;
            double lastOut = 0.0;
            std::set<Endpoint> sentTo;
        };
        struct Nat {
            NatSettings s;
            std::vector<MapEntry> maps;
            uint16_t nextPort;
            size_t nextIp;
        };
        struct Flight {
            double due;
            Endpoint from, to;
            std::vector<uint8_t> bytes;
            int fromNat;   ///< -1 public, -2 inside a LAN, else the NAT it left
        };

        bool Live(const Nat& n, const MapEntry& m, double now) const { return now - m.lastOut <= n.s.mappingTimeout; }

        MapEntry& MapFor(Nat& n, Endpoint inside, Endpoint to, double now) {
            for (MapEntry& m : n.maps)
                if (m.inside == inside && (n.s.mapping == Mapping::Independent || m.destination == to) && Live(n, m, now)) return m;
            std::erase_if(n.maps, [&](const MapEntry& m) { return !Live(n, m, now); });
            MapEntry m;
            m.inside = inside;
            m.destination = to;
            const size_t ipIndex = n.s.mapping == Mapping::Symmetric ? n.nextIp++ % n.s.outsideIps.size() : 0;
            m.outside = {n.s.outsideIps[ipIndex], n.nextPort++};
            n.maps.push_back(std::move(m));
            return n.maps.back();
        }

        void Arrive(Flight& f) {
            const auto socket = m_Sockets.find(f.to);
            if (socket != m_Sockets.end() && (socket->second < 0 || f.fromNat == -2)) {   // public, or a LAN neighbour
                Deliver(f.to, f.from, std::move(f.bytes));
                return;
            }
            for (size_t k = 0; k < m_Nats.size(); ++k) {
                Nat& n = m_Nats[k];
                if (std::find(n.s.outsideIps.begin(), n.s.outsideIps.end(), f.to.ip) == n.s.outsideIps.end()) continue;
                if (f.fromNat == static_cast<int>(k) && !n.s.hairpin) return;
                for (const MapEntry& m : n.maps) {
                    if (m.outside != f.to || !Live(n, m, f.due)) continue;
                    bool pass = n.s.filtering == Filtering::Independent;
                    for (const Endpoint& e : m.sentTo)
                        if ((n.s.filtering == Filtering::Address && e.ip == f.from.ip) || (n.s.filtering == Filtering::Port && e == f.from)) pass = true;
                    if (pass) Deliver(m.inside, f.from, std::move(f.bytes));
                    return;
                }
                return;
            }
        }

        void Deliver(Endpoint to, Endpoint from, std::vector<uint8_t> bytes) {
            m_Inbox[to].push_back({from, std::move(bytes)});
            ++m_Delivered;
        }

        std::vector<Nat> m_Nats;
        std::map<Endpoint, int> m_Sockets;
        std::vector<Flight> m_Flight;
        std::map<Endpoint, std::vector<Datagram>> m_Inbox;
        uint64_t m_Delivered = 0;
    };

    /// A fake STUN server on a public socket: answers every Binding request with the source it saw.
    inline void ServeStun(NatSim& sim, Endpoint server, double now) {
        for (const Datagram& d : sim.Receive(server)) {
            vortex::net::StunTransaction t{};
            if (!vortex::net::ParseStunBindingRequest(d.bytes, t)) continue;
            sim.Send(server, d.address, vortex::net::MakeStunBindingResponse(t, d.address), now);
        }
    }
}
