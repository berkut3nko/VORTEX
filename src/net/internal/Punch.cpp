module;

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

module vortex.net;

import vortex.core;

namespace vortex::net {

    namespace {
        using Clock = std::chrono::steady_clock;

        /// Lines typed or pasted on stdin, read by a thread of their own (a blocked read never holds the
        /// punching loop). The thread is detached: it may still wait for input when the program ends.
        struct StdinLines {
            std::mutex mutex;
            std::deque<std::string> lines;
        };

        /// One read of file descriptor 0, never through std::cin / stdio: a thread blocked in getline
        /// holds stdin's stdio lock, and glibc's exit() takes every stream's lock (2.41 on), so the
        /// program would hang at its end, after "Punch: the pair test is done".
        long ReadFd0(char* buffer, unsigned size) {
#if defined(_WIN32)
            return _read(0, buffer, size);
#else
            return static_cast<long>(::read(0, buffer, size));
#endif
        }

        std::shared_ptr<StdinLines> ReadStdin() {
            auto shared = std::make_shared<StdinLines>();
            std::thread([shared] {
                std::string pending;
                char buffer[4096];
                for (;;) {
                    const long n = ReadFd0(buffer, sizeof(buffer));
                    if (n <= 0) break;   // end of input or an error: no more lines
                    pending.append(buffer, static_cast<size_t>(n));
                    for (size_t end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                        std::lock_guard lock(shared->mutex);
                        shared->lines.push_back(pending.substr(0, end));
                        pending.erase(0, end + 1);
                    }
                }
                if (!pending.empty()) {
                    std::lock_guard lock(shared->mutex);
                    shared->lines.push_back(std::move(pending));
                }
            }).detach();
            return shared;
        }

        std::optional<std::string> NextLine(StdinLines& in) {
            std::lock_guard lock(in.mutex);
            if (in.lines.empty()) return std::nullopt;
            std::string line = std::move(in.lines.front());
            in.lines.pop_front();
            return line;
        }

        std::string UtcNow() {
            return std::format("{:%H:%M:%S} UTC", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        }

        uint64_t RandomSeed() {
            std::random_device device;
            return static_cast<uint64_t>(device()) << 32 | device();
        }

        struct Options {
            bool probe = false, punch = false;
            Token token{};
            bool haveToken = false;
            std::vector<std::string> stun;
            CandidateKinds kinds;
            std::string bind;
            uint16_t port = 0;
            std::optional<Endpoint> given;
        };

        bool Parse(int argc, const char* const* argv, PunchRole role, Options& o, std::string& error) {
            NatFlags nat;
            if (!ParseNatFlags(argc, argv, role, nat, error)) return false;
            o.stun = std::move(nat.stun);
            o.kinds = nat.kinds;
            o.given = nat.given;
            for (int i = 1; i < argc; ++i) {
                const std::string_view a = argv[i];
                const auto value = [&](std::string& out) {
                    if (i + 1 >= argc) {
                        error = std::format("{} needs a value", a);
                        return false;
                    }
                    out = argv[++i];
                    return true;
                };
                std::string v;
                if (a == "--nat-probe") {
                    o.probe = true;
                } else if (a == "--punch-test") {
                    o.punch = true;
                } else if (a == "--token") {
                    if (!value(v)) return false;
                    if (!ParseToken(v, o.token)) {
                        error = "--token expects 32 hex digits";
                        return false;
                    }
                    o.haveToken = true;
                } else if (a == "--stun" || a == "--candidates" || (a == "--public" && role == PunchRole::Server)) {
                    ++i;   // ParseNatFlags took it
                } else if (a == "--bind") {
                    if (!value(v)) return false;
                    o.bind = v;
                } else if ((a == "--listen" && role == PunchRole::Server) || (a == "--port" && role == PunchRole::Client)) {
                    if (!value(v)) return false;
                    std::string host;
                    if (!ParseEndpoint("x:" + v, 0, host, o.port)) {
                        error = std::format("{} expects a UDP port (1-65535)", a);
                        return false;
                    }
                }
            }
            if (o.port == kDefaultPort) {
                error = std::format("UDP port {} is the live server's: pick another", kDefaultPort);
                return false;
            }
            if (o.punch && !o.haveToken) {
                error = "--punch-test needs --token HEX (the same on both sides)";
                return false;
            }
            return true;
        }

        int NatProbe(const Options& o) {
            std::string error;
            const std::unique_ptr<EnetTransport> transport = EnetTransport::Open(o.bind, o.port, {}, &error);
            if (!transport) {
                Log::Error("NAT: {}", error);
                return 1;
            }
            StunProbe stun(ResolveStunServers(o.stun), RandomSeed());
            const Clock::time_point start = Clock::now();
            const auto now = [&] { return std::chrono::duration<double>(Clock::now() - start).count(); };
            std::vector<Datagram> out;
            while (!stun.Settled() && now() < 10.0) {
                TransportEvent e;
                while (transport->Poll(e)) {}
                for (const Datagram& d : transport->TakeRaw()) stun.OnDatagram(d, now());
                out.clear();
                stun.Update(now(), out);
                for (const Datagram& d : out) transport->SendRaw(d.address, d.bytes);
                transport->Flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            for (size_t i = 0; i < stun.Servers().size(); ++i) {
                const std::optional<Endpoint>& m = stun.Mapped()[i];
                Log::Info("NAT: STUN server {} answered {}", FormatEndpoint(stun.Servers()[i]), m ? FormatEndpoint(*m) : std::string("nothing"));
            }
            for (const LocalInterface& i : LocalInterfaces())
                Log::Info("NAT: interface {} {}/{}{}", i.name, FormatIpv4(i.ip), FormatIpv4(i.mask), IsLanInterface(i) ? " (lan candidate)" : "");
            const std::optional<Endpoint> outside = stun.Outside();
            Log::Info("NAT: outside {}, class {}", outside ? FormatEndpoint(*outside) : std::string("-"), ToString(stun.Class()));
            Log::Info("NAT: UDP port {}", transport->Port());
            return 0;
        }

        int PairTest(const Options& o, PunchRole role) {
            std::string error;
            EnetSettings es;
            es.maxPeers = 4;
            const std::unique_ptr<EnetTransport> transport =
                role == PunchRole::Server ? EnetTransport::Listen(o.bind, o.port, es, &error) : EnetTransport::Open(o.bind, o.port, es, &error);
            if (!transport) {
                Log::Error("Punch: {}", error);
                return 1;
            }
            const bool server = role == PunchRole::Server;
            Log::Info("Punch: the {} side of the pair test on UDP port {}", server ? "server" : "client", transport->Port());
            PunchTestSettings settings;
            settings.role = role;
            settings.token = o.token;
            settings.stunServers = ResolveStunServers(o.stun);
            settings.interfaces = LocalInterfaces();
            settings.port = transport->Port();
            settings.given = o.given;
            settings.kinds = o.kinds;
            settings.seed = RandomSeed();
            const Clock::time_point start = Clock::now();
            const auto now = [&] { return std::chrono::duration<double>(Clock::now() - start).count(); };
            PunchTest test(settings, now());
            const std::shared_ptr<StdinLines> input = ReadStdin();
            bool linePrinted = false;
            std::optional<double> connectStart, connectedAt, outcomeAt;
            std::vector<Datagram> out;
            for (;;) {
                const double t = now();
                TransportEvent e;
                while (transport->Poll(e)) {
                    if (e.type == TransportEvent::Type::Connected && !connectedAt) {
                        connectedAt = t;
                        if (server)
                            Log::Info("Punch: ENet peer {} connected over the punched path", transport->PeerAddress(e.peer));
                        else
                            Log::Info("Punch: ENet connected to {} in {:.0f} ms", FormatEndpoint(test.ReachedAt().at), (t - *connectStart) * 1000.0);
                        if (!server) transport->Disconnect(e.peer);
                    } else if (e.type == TransportEvent::Type::Disconnected && server && connectedAt) {
                        Log::Info("Punch: the pair test is done");
                        return 0;
                    }
                }
                for (const Datagram& d : transport->TakeRaw()) test.OnDatagram(d, t);
                while (const std::optional<std::string> line = NextLine(*input)) {
                    if (line->find_first_not_of(" \t\r\n") == std::string::npos) continue;
                    if (test.Paste(*line, t, error))
                        Log::Info("Punch: line pasted at {} ({:.0f} s after start)", UtcNow(), t);
                    else
                        Log::Warn("Punch: that line was not taken: {}", error);
                }
                out.clear();
                test.Update(t, out);
                for (const Datagram& d : out) transport->SendRaw(d.address, d.bytes);
                for (const std::string& line : test.TakeLog()) Log::Info("{}", line);
                if (!linePrinted && !test.Line().empty()) {
                    linePrinted = true;
                    Log::Info("Punch: copy this line into the other program (it reads stdin; press Enter after pasting):");
                    std::printf("%s\n", test.Line().c_str());
                    std::fflush(stdout);
                }
                const PunchTest::Phase phase = test.CurrentPhase();
                if ((phase == PunchTest::Phase::Reached || phase == PunchTest::Phase::Failed) && !outcomeAt) outcomeAt = t;
                if (!server) {
                    if (phase == PunchTest::Phase::Failed) return 1;
                    if (phase == PunchTest::Phase::Reached && !connectStart) {
                        connectStart = t;
                        if (!transport->ConnectTo(test.ReachedAt().at, &error)) {
                            Log::Error("Punch: {}", error);
                            return 1;
                        }
                    }
                    if (connectedAt && t - *connectedAt > 0.5) {
                        Log::Info("Punch: the pair test is done");
                        return 0;
                    }
                    if (connectStart && !connectedAt && t - *connectStart > 10.0) {
                        Log::Error("Punch: ENet did not connect to {} within 10 s of the reach", FormatEndpoint(test.ReachedAt().at));
                        return 1;
                    }
                } else {
                    if (connectedAt && t - *connectedAt > 3.0) {
                        Log::Info("Punch: the pair test is done");
                        return 0;
                    }
                    // The client connects after its own reach: wait a while past this side's outcome.
                    if (outcomeAt && !connectedAt && t - *outcomeAt > 20.0) {
                        Log::Error("Punch: no ENet connection from the client within 20 s of this side's {}",
                                   phase == PunchTest::Phase::Reached ? "reach" : "failure");
                        return 1;
                    }
                }
                if (phase == PunchTest::Phase::WaitingForPaste && t > 1800.0) {
                    Log::Error("Punch: no line was pasted within 30 minutes");
                    return 1;
                }
                transport->Flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }

    bool ParseNatFlags(int argc, const char* const* argv, PunchRole role, NatFlags& out, std::string& error) {
        NatFlags flags;
        for (int i = 1; i < argc; ++i) {
            std::string_view a = argv[i];
            std::string value;
            bool hasValue = false;
            if (const size_t eq = a.find('='); a.starts_with("--") && eq != std::string_view::npos) {
                value = std::string(a.substr(eq + 1));
                a = a.substr(0, eq);
                hasValue = true;
            }
            if (a != "--stun" && a != "--candidates" && a != "--public") continue;
            if (!hasValue) {
                if (i + 1 >= argc || std::string_view(argv[i + 1]).starts_with("--")) {
                    error = std::format("{} needs a value", a);
                    return false;
                }
                value = argv[++i];
            }
            if (a == "--stun") {
                flags.stun.push_back(value);
            } else if (a == "--candidates") {
                if (!ParseCandidateKinds(value, flags.kinds)) {
                    error = "--candidates expects a list of lan, stun, given";
                    return false;
                }
            } else if (role == PunchRole::Server) {
                Endpoint e;
                if (!ParseIpv4Endpoint(value, e)) {
                    error = "--public expects A.B.C.D:PORT";
                    return false;
                }
                flags.given = e;
            } else {
                error = "--public is the server's";
                return false;
            }
        }
        if (flags.stun.empty())
            for (const char* s : kDefaultStunServers) flags.stun.emplace_back(s);
        out = std::move(flags);
        return true;
    }

    std::vector<Endpoint> ResolveStunServers(const std::vector<std::string>& names) {
        std::vector<Endpoint> out;
        for (const std::string& name : names) {
            Endpoint e;
            std::string why;
            if (ResolveEndpoint(name, 3478, e, &why)) {
                out.push_back(e);
                Log::Info("NAT: STUN server {} is {}", name, FormatEndpoint(e));
            } else {
                Log::Warn("NAT: STUN server {}: {}", name, why);
            }
        }
        return out;
    }

    bool WantsNatTool(int argc, const char* const* argv) {
        for (int i = 1; i < argc; ++i) {
            const std::string_view a = argv[i];
            if (a == "--nat-probe" || a == "--punch-test") return true;
        }
        return false;
    }

    int RunNatTool(int argc, const char* const* argv, PunchRole role) {
        Options o;
        std::string error;
        if (!Parse(argc, argv, role, o, error)) {
            Log::Error("{}", error);
            return 2;
        }
        if (o.punch) return PairTest(o, role);
        return NatProbe(o);
    }
}
