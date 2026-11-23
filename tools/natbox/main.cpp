// vortex_natbox (docs/design/rendezvous.md "Tests", gate G20): NATs of every kind on one Linux machine, so
// the real programs, the real ENet path and the real Worker (local.mjs) are tested through them. A test
// tool, never shipped; Linux only (all of 127.0.0.0/8 is local there).
//
//   vortex_natbox --fake-stun A.B.C.D:PORT [--parent-pid PID]
//       answers every STUN Binding request with the source address it came from.
//   vortex_natbox --name NAME --outside-ips IP[,IP...] --mapping eim|symmetric
//                 --filtering independent|address|port --stun-server A:P [--stun-server A:P]
//                 [--parent-pid PID] [--seconds S]
//       one emulated NAT in front of one program. For every remote endpoint R the program may address, the
//       box has an alias socket A(R) on 127.0.0.1; the program only ever sends to aliases (its --stun names the
//       alias of a STUN server; the rendezvous hands out the aliases of the peer's candidates, which it asks
//       for on the control socket: "alias A.B.C.D:P" -> "alias A.B.C.D:P 127.0.0.1:Q"). A datagram to A(R)
//       leaves from an outside socket (eim: one for every destination; symmetric: one per destination, on the
//       outside IPs in turn) toward R; a datagram that arrives at an outside socket from O passes the
//       filtering rule (independent: anyone; address: an IP this socket has sent to; port: an ip:port it has
//       sent to) and reaches the program from A(O). Only the first program that used an alias is served.
//
// Logs the Build: line first, then `Natbox: NAME ready: control 127.0.0.1:C, stun alias 127.0.0.1:S[,...]`
// (G20 reads it) and at exit `Natbox: NAME passed N out, M in; dropped K by filtering, J from another inside
// address`. Exit 0 on SIGTERM / SIGINT, after --seconds or when --parent-pid is gone; 2 on a bad command line
// or a socket that cannot be made.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import vortex.core;
import vortex.net;

using namespace vortex;
using net::Endpoint;

namespace {
    std::atomic<bool> g_Stop{false};
    void OnSignal(int) { g_Stop = true; }

    bool IsLoopbackIp(uint32_t ip) { return (ip >> 24) == 127; }

    /// A non-blocking UDP socket bound to ip:port (port 0: any); -1 on failure.
    int MakeSocket(uint32_t ip, uint16_t port, Endpoint* bound = nullptr) {
        const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(ip);
        a.sin_port = htons(port);
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
            ::close(fd);
            return -1;
        }
        if (bound != nullptr) {
            socklen_t len = sizeof(a);
            ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
            *bound = Endpoint{ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
        }
        return fd;
    }

    void SendTo(int fd, Endpoint to, std::span<const uint8_t> bytes) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(to.ip);
        a.sin_port = htons(to.port);
        (void)::sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    }

    /// One datagram from fd, or nullopt when none is waiting.
    std::optional<net::Datagram> Receive(int fd) {
        uint8_t buffer[2048];
        sockaddr_in a{};
        socklen_t len = sizeof(a);
        const ssize_t n = ::recvfrom(fd, buffer, sizeof(buffer), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&a), &len);
        if (n < 0) return std::nullopt;
        net::Datagram d;
        d.address = Endpoint{ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
        d.bytes.assign(buffer, buffer + n);
        return d;
    }

    struct Options {
        std::optional<Endpoint> fakeStun;
        std::string name;
        std::vector<uint32_t> outsideIps;
        bool symmetric = false;
        enum class Filter { Independent, Address, Port } filtering = Filter::Port;
        bool haveMapping = false, haveFiltering = false;
        std::vector<Endpoint> stunServers;
        long parentPid = 0;
        double seconds = 0.0;
    };

    bool Parse(int argc, char** argv, Options& o, std::string& error) {
        for (int i = 1; i < argc; ++i) {
            const std::string_view a = argv[i];
            if (i + 1 >= argc) {
                error = std::string(a) + " needs a value";
                return false;
            }
            const std::string v = argv[++i];
            Endpoint e;
            if (a == "--fake-stun") {
                if (!net::ParseIpv4Endpoint(v, e) || !IsLoopbackIp(e.ip) || e.port < 1024) {
                    error = "--fake-stun expects 127.A.B.C:PORT with a port of 1024 or more";
                    return false;
                }
                o.fakeStun = e;
            } else if (a == "--name") {
                o.name = v;
            } else if (a == "--outside-ips") {
                size_t start = 0;
                while (start <= v.size()) {
                    const size_t comma = std::min(v.find(',', start), v.size());
                    uint32_t ip = 0;
                    if (!net::ParseIpv4(std::string_view(v).substr(start, comma - start), ip) || !IsLoopbackIp(ip) || ip == net::Ipv4(127, 0, 0, 1)) {
                        error = "--outside-ips expects addresses in 127.0.0.0/8 other than 127.0.0.1";
                        return false;
                    }
                    o.outsideIps.push_back(ip);
                    start = comma + 1;
                }
            } else if (a == "--mapping") {
                if (v != "eim" && v != "symmetric") {
                    error = "--mapping expects eim or symmetric";
                    return false;
                }
                o.symmetric = v == "symmetric";
                o.haveMapping = true;
            } else if (a == "--filtering") {
                if (v == "independent") o.filtering = Options::Filter::Independent;
                else if (v == "address") o.filtering = Options::Filter::Address;
                else if (v == "port") o.filtering = Options::Filter::Port;
                else {
                    error = "--filtering expects independent, address or port";
                    return false;
                }
                o.haveFiltering = true;
            } else if (a == "--stun-server") {
                if (!net::ParseIpv4Endpoint(v, e)) {
                    error = "--stun-server expects A.B.C.D:PORT";
                    return false;
                }
                o.stunServers.push_back(e);
            } else if (a == "--parent-pid") {
                o.parentPid = std::atol(v.c_str());
                if (o.parentPid <= 0) {
                    error = "--parent-pid expects a process id";
                    return false;
                }
            } else if (a == "--seconds") {
                o.seconds = std::atof(v.c_str());
            } else {
                error = "unknown argument " + std::string(a);
                return false;
            }
        }
        if (o.fakeStun) return true;
        if (o.name.empty() || o.outsideIps.empty() || !o.haveMapping || !o.haveFiltering || o.stunServers.empty()) {
            error = "a box needs --name, --outside-ips, --mapping, --filtering and --stun-server";
            return false;
        }
        return true;
    }

    /// Whether to stop: a signal, --seconds, or the parent gone (checked once a second).
    struct StopCheck {
        long parentPid = 0;
        double seconds = 0.0;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now(), lastParentCheck = start;
        bool operator()() {
            if (g_Stop) return true;
            const auto now = std::chrono::steady_clock::now();
            if (seconds > 0.0 && std::chrono::duration<double>(now - start).count() >= seconds) return true;
            if (parentPid > 0 && now - lastParentCheck >= std::chrono::seconds(1)) {
                lastParentCheck = now;
                if (::kill(static_cast<pid_t>(parentPid), 0) != 0 && errno == ESRCH) return true;
            }
            return false;
        }
    };

    int FakeStun(const Options& o, StopCheck& stop) {
        const int fd = MakeSocket(o.fakeStun->ip, o.fakeStun->port);
        if (fd < 0) {
            Log::Error("Natbox: cannot bind the fake STUN server to {}: {}", net::FormatEndpoint(*o.fakeStun), std::strerror(errno));
            return 2;
        }
        Log::Info("Natbox: fake STUN on {}", net::FormatEndpoint(*o.fakeStun));
        uint64_t answered = 0;
        while (!stop()) {
            pollfd p{fd, POLLIN, 0};
            ::poll(&p, 1, 100);
            while (const std::optional<net::Datagram> d = Receive(fd)) {
                net::StunTransaction t{};
                if (!net::ParseStunBindingRequest(d->bytes, t)) continue;
                SendTo(fd, d->address, net::MakeStunBindingResponse(t, d->address));
                ++answered;
            }
        }
        Log::Info("Natbox: fake STUN answered {} request(s)", answered);
        ::close(fd);
        return 0;
    }

    class Box {
    public:
        explicit Box(const Options& o) : m_Options(o) {}

        ~Box() {
            for (const auto& [fd, remote] : m_RemoteOf) ::close(fd);
            for (const Outside& out : m_Outs) ::close(out.fd);
            if (m_Control >= 0) ::close(m_Control);
        }

        int Run(StopCheck& stop) {
            Endpoint control;
            m_Control = MakeSocket(net::Ipv4(127, 0, 0, 1), 0, &control);
            if (m_Control < 0) {
                Log::Error("Natbox: {}: cannot make the control socket: {}", m_Options.name, std::strerror(errno));
                return 2;
            }
            std::string aliases;
            for (const Endpoint& s : m_Options.stunServers) {
                const int fd = AliasOf(s);
                if (fd < 0) return 2;
                aliases += (aliases.empty() ? "" : ",") + net::FormatEndpoint(m_LocalOf[fd]);
            }
            Log::Info("Natbox: {} ready: control {}, stun alias {}", m_Options.name, net::FormatEndpoint(control), aliases);

            std::vector<pollfd> polled;
            while (!stop()) {
                polled.clear();
                polled.push_back({m_Control, POLLIN, 0});
                for (const auto& [fd, remote] : m_RemoteOf) polled.push_back({fd, POLLIN, 0});
                for (const Outside& out : m_Outs) polled.push_back({out.fd, POLLIN, 0});
                ::poll(polled.data(), polled.size(), 100);
                for (const pollfd& p : polled) {
                    if ((p.revents & POLLIN) == 0) continue;
                    if (p.fd == m_Control) OnControl();
                    else if (m_RemoteOf.contains(p.fd)) OnAlias(p.fd);
                    else OnOutside(p.fd);
                }
            }
            Log::Info("Natbox: {} passed {} out, {} in; dropped {} by filtering, {} from another inside address", m_Options.name, m_Out, m_In,
                      m_Filtered, m_Strangers);
            return 0;
        }

    private:
        struct Outside {
            int fd = -1;
            std::set<Endpoint> sentTo;
            std::set<uint32_t> sentToIps;
        };

        /// The alias socket of remote R (made on demand); -1 when it cannot be made.
        int AliasOf(Endpoint remote) {
            if (const auto it = m_AliasOf.find(remote); it != m_AliasOf.end()) return it->second;
            Endpoint local;
            const int fd = MakeSocket(net::Ipv4(127, 0, 0, 1), 0, &local);
            if (fd < 0) {
                Log::Error("Natbox: {}: cannot make an alias socket: {}", m_Options.name, std::strerror(errno));
                return -1;
            }
            m_AliasOf[remote] = fd;
            m_RemoteOf[fd] = remote;
            m_LocalOf[fd] = local;
            return fd;
        }

        /// The outside socket a datagram to `remote` leaves from (made on demand); nullptr when it cannot be made.
        Outside* OutsideFor(Endpoint remote) {
            size_t index = 0;
            if (!m_Options.symmetric) {
                if (m_Outs.empty() && !AddOutside()) return nullptr;
                index = 0;
            } else if (const auto it = m_OutFor.find(remote); it != m_OutFor.end()) {
                index = it->second;
            } else {
                if (!AddOutside()) return nullptr;
                index = m_Outs.size() - 1;
                m_OutFor[remote] = index;
            }
            return &m_Outs[index];
        }

        bool AddOutside() {
            const uint32_t ip = m_Options.outsideIps[m_Outs.size() % m_Options.outsideIps.size()];
            const int fd = MakeSocket(ip, 0);
            if (fd < 0) {
                Log::Warn("Natbox: {}: cannot make an outside socket on {}: {}", m_Options.name, net::FormatIpv4(ip), std::strerror(errno));
                return false;
            }
            m_Outs.push_back({fd, {}, {}});
            return true;
        }

        void OnControl() {
            while (const std::optional<net::Datagram> d = Receive(m_Control)) {
                const std::string text(d->bytes.begin(), d->bytes.end());
                Endpoint e;
                std::string reply;
                if (text.starts_with("alias ") && net::ParseIpv4Endpoint(std::string_view(text).substr(6), e)) {
                    const int fd = AliasOf(e);
                    reply = fd >= 0 ? "alias " + net::FormatEndpoint(e) + " " + net::FormatEndpoint(m_LocalOf[fd]) : "error no socket";
                } else {
                    reply = "error expected 'alias A.B.C.D:P'";
                }
                SendTo(m_Control, d->address, std::span(reinterpret_cast<const uint8_t*>(reply.data()), reply.size()));
            }
        }

        // From the program: out toward the alias's remote.
        void OnAlias(int fd) {
            const Endpoint remote = m_RemoteOf[fd];
            while (const std::optional<net::Datagram> d = Receive(fd)) {
                if (!m_Inside) m_Inside = d->address;
                if (d->address != *m_Inside) {
                    ++m_Strangers;
                    continue;
                }
                Outside* out = OutsideFor(remote);
                if (out == nullptr) continue;
                out->sentTo.insert(remote);
                out->sentToIps.insert(remote.ip);
                SendTo(out->fd, remote, d->bytes);
                ++m_Out;
            }
        }

        // From outside: the filtering rule, then to the program from the alias of the source.
        void OnOutside(int fd) {
            Outside* out = nullptr;
            for (Outside& o : m_Outs)
                if (o.fd == fd) out = &o;
            if (out == nullptr) return;
            while (const std::optional<net::Datagram> d = Receive(fd)) {
                const Endpoint from = d->address;
                bool pass = true;
                if (m_Options.filtering == Options::Filter::Address) pass = out->sentToIps.contains(from.ip);
                else if (m_Options.filtering == Options::Filter::Port) pass = out->sentTo.contains(from);
                if (!pass) {
                    ++m_Filtered;
                    continue;
                }
                if (!m_Inside) continue;
                const int alias = AliasOf(from);
                if (alias < 0) continue;
                SendTo(alias, *m_Inside, d->bytes);
                ++m_In;
            }
        }

        const Options& m_Options;
        int m_Control = -1;
        std::optional<Endpoint> m_Inside;
        std::map<Endpoint, int> m_AliasOf;
        std::map<int, Endpoint> m_RemoteOf, m_LocalOf;
        std::vector<Outside> m_Outs;
        std::map<Endpoint, size_t> m_OutFor;
        uint64_t m_Out = 0, m_In = 0, m_Filtered = 0, m_Strangers = 0;
    };
}

int main(int argc, char** argv) {
    Log::Init();
    Log::Info("Build: {}", DescribeBuild());
    Options o;
    std::string error;
    if (!Parse(argc, argv, o, error)) {
        Log::Error("{}", error);
        std::fprintf(stderr,
                     "usage: vortex_natbox --fake-stun A.B.C.D:PORT [--parent-pid PID]\n"
                     "       vortex_natbox --name NAME --outside-ips IP[,IP...] --mapping eim|symmetric --filtering independent|address|port "
                     "--stun-server A:P [--stun-server A:P] [--parent-pid PID] [--seconds S]\n");
        return 2;
    }
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    if (o.parentPid > 0) prctl(PR_SET_PDEATHSIG, SIGTERM);
    StopCheck stop;
    stop.parentPid = o.parentPid;
    stop.seconds = o.seconds;
    if (o.fakeStun) return FakeStun(o, stop);
    Box box(o);
    return box.Run(stop);
}
