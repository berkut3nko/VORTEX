module;

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

module vortex.net;

import vortex.core;

namespace vortex::net {

    namespace {
        using Clock = std::chrono::steady_clock;

        /// Lines typed or pasted on stdin, read by a thread of their own (a blocked read never holds the
        /// punching loop). The thread is detached: it may still wait in getline when the program ends.
        struct StdinLines {
            std::mutex mutex;
            std::deque<std::string> lines;
        };

        std::shared_ptr<StdinLines> ReadStdin() {
            auto shared = std::make_shared<StdinLines>();
            std::thread([shared] {
                std::string line;
                while (std::getline(std::cin, line)) {
                    std::lock_guard lock(shared->mutex);
                    shared->lines.push_back(line);
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
                } else if (a == "--stun") {
                    if (!value(v)) return false;
                    o.stun.push_back(v);
                } else if (a == "--candidates") {
                    if (!value(v)) return false;
                    if (!ParseCandidateKinds(v, o.kinds)) {
                        error = "--candidates expects a list of lan, stun, given";
                        return false;
                    }
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
                } else if (a == "--public" && role == PunchRole::Server) {
                    if (!value(v)) return false;
                    Endpoint e;
                    if (!ParseIpv4Endpoint(v, e)) {
                        error = "--public expects A.B.C.D:PORT";
                        return false;
                    }
                    o.given = e;
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
            if (o.stun.empty())
                for (const char* s : kDefaultStunServers) o.stun.emplace_back(s);
            return true;
        }

        std::vector<Endpoint> ResolveStun(const std::vector<std::string>& names) {
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

        int NatProbe(const Options& o) {
            std::string error;
            const std::unique_ptr<EnetTransport> transport = EnetTransport::Open(o.bind, o.port, {}, &error);
            if (!transport) {
                Log::Error("NAT: {}", error);
                return 1;
            }
            StunProbe stun(ResolveStun(o.stun), RandomSeed());
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
            Log::Info("NAT: outside {}, class {} (UDP port {})", outside ? FormatEndpoint(*outside) : std::string("-"), ToString(stun.Class()),
                      transport->Port());
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
            settings.stunServers = ResolveStun(o.stun);
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
