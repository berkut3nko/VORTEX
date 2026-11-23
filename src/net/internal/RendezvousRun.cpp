module;

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module vortex.net;

import vortex.core;

// docs/design/rendezvous.md [nat-R1]: what the programs run. RendezvousHost: vortex_server --rendezvous
// (STUN, the room, the server's side of every join over the listening socket, inside the server's pass).
// JoinThroughRendezvous: Shredborne --join (STUN, the join, the client's punching; blocks before the
// session starts).

namespace vortex::net {

    namespace {
        std::vector<LocalInterface> LanOnly(const std::vector<LocalInterface>& interfaces) {
            std::vector<LocalInterface> out;
            for (const LocalInterface& i : interfaces)
                if (IsLanInterface(i)) out.push_back(i);
            return out;
        }

        std::string OutsideText(const StunProbe& stun) {
            const std::optional<Endpoint> outside = stun.Outside();
            return outside ? FormatEndpoint(*outside) : std::string("-");
        }

        size_t CandidateCount(const std::string& text) {
            std::vector<Candidate> c;
            return ParseCandidates(text, c) ? c.size() : 0;
        }
    }

    // --- RendezvousHost ----------------------------------------------------------------------------

    RendezvousHost::RendezvousHost(HttpsClient& https, RendezvousHostSettings settings, double now)
        : m_Settings(std::move(settings)),
          m_Stun(m_Settings.stunServers, m_Settings.seed * 2 + 1),
          m_Rendezvous(https, m_Settings.rendezvous),
          m_Traversal(m_Settings.rendezvous.token, LanOnly(m_Settings.interfaces), TraversalLimits{}, m_Settings.seed * 2 + 3),
          m_Start(now) {
        Log::Info("Rendezvous: {}, room {}, {} STUN server(s)", m_Settings.rendezvous.url, RoomHex(m_Settings.rendezvous.token).substr(0, 8),
                  m_Settings.stunServers.size());
    }

    void RendezvousHost::Update(double now, EnetTransport& udp) {
        if (m_ClosedAt >= 0.0) {
            m_Rendezvous.Update(now, m_Candidates, m_Stun.Class());
            for (const std::string& line : m_Rendezvous.TakeLog()) Log::Info("{}", line);
            return;
        }
        for (const Datagram& d : udp.TakeRaw())
            if (!m_Stun.OnDatagram(d, now)) m_Traversal.OnDatagram(d, now);
        m_Out.clear();
        m_Stun.Update(now, m_Out);

        if (!m_Gathered && (m_Stun.Settled() || now - m_Start >= 7.0)) {
            m_Gathered = true;
            Log::Info("NAT: outside {}, class {}", OutsideText(m_Stun), ToString(m_Stun.Class()));
        }
        if (m_Stun.TakeChange()) Log::Info("NAT: the outside address changed to {}", OutsideText(m_Stun));

        if (m_Gathered) {
            m_Candidates = ChooseCandidates(PunchRole::Server, m_Settings.interfaces, m_Settings.port, m_Settings.kinds, m_Settings.given,
                                            m_Stun.Mapped());
            m_Rendezvous.Update(now, m_Candidates, m_Stun.Class());
        }

        for (const ServerJoin& join : m_Rendezvous.TakeJoins()) {
            switch (m_Traversal.AddJoin(join, now)) {
                case AddJoinResult::Added:
                    Log::Info("Punch: join {} from the Worker, {} candidate(s){}", join.id, CandidateCount(join.candidates),
                              join.verified ? "" : ", unverified");
                    break;
                case AddJoinResult::DuplicateNonce: Log::Info("Punch: join {} refused: duplicate nonce", join.id); break;
                case AddJoinResult::Busy: Log::Info("Punch: join {} refused: 4 joins at once", join.id); break;
                case AddJoinResult::BadCandidates: Log::Info("Punch: join {} refused: bad candidates", join.id); break;
            }
        }

        m_Traversal.Update(now, m_Out);
        for (const ServerJoinOutcome& o : m_Traversal.TakeOutcomes()) {
            if (o.reached)
                Log::Info("Punch: join {} reached {} ({}) in {:.0f} ms", o.id, FormatEndpoint(o.at.at), ToString(o.at.kind), o.seconds * 1000.0);
            else
                Log::Info("Punch: join {} failed after {:.0f} s: {}", o.id, o.seconds,
                          o.heard ? "the client was heard, but the two networks cannot meet"
                                  : "no datagram from the client arrived: the client may have given up, or the two networks cannot meet (compare both sides' --nat-probe)");
        }

        for (const Datagram& d : m_Out) udp.SendRaw(d.address, d.bytes);
        for (const std::string& line : m_Rendezvous.TakeLog()) Log::Info("{}", line);
    }

    void RendezvousHost::Close(double now) {
        if (m_ClosedAt >= 0.0) return;
        m_Rendezvous.Close(now);
        m_ClosedAt = now;
    }

    bool RendezvousHost::Closed(double now) {
        if (m_ClosedAt < 0.0) return false;
        if (m_Rendezvous.State() == RendezvousState::Closed) return true;
        if (now - m_ClosedAt >= 2.0) {
            if (!m_CloseTimedOut) Log::Warn("Rendezvous: the delete did not finish within 2 s");
            m_CloseTimedOut = true;
            return true;
        }
        return false;
    }

    // --- JoinThroughRendezvous ---------------------------------------------------------------------

    std::optional<Endpoint> JoinThroughRendezvous(HttpsClient& https, EnetTransport& udp, const JoinSettings& settings, std::string& error) {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point start = Clock::now();
        const auto now = [&] { return std::chrono::duration<double>(Clock::now() - start).count(); };

        StunProbe stun(settings.stunServers, settings.seed * 2 + 1);
        std::optional<ClientTraversal> traversal;
        std::vector<Datagram> out;
        const auto pump = [&] {
            const double t = now();
            TransportEvent e;
            while (udp.Poll(e)) {}
            for (const Datagram& d : udp.TakeRaw())
                if (!stun.OnDatagram(d, t) && traversal) traversal->OnDatagram(d, t);
            out.clear();
            stun.Update(t, out);
            if (traversal) traversal->Update(t, out);
            for (const Datagram& d : out) udp.SendRaw(d.address, d.bytes);
            udp.Flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        };
        const auto pumpFor = [&](double seconds) {
            const double until = now() + seconds;
            while (now() < until) pump();
        };

        // 1. STUN on the transport's own socket: the outside address ENet will use.
        while (!stun.Settled() && now() < 7.0) pump();
        Log::Info("NAT: outside {}, class {}", OutsideText(stun), ToString(stun.Class()));

        const std::vector<Candidate> candidates =
            ChooseCandidates(PunchRole::Client, settings.interfaces, settings.port, settings.kinds, std::nullopt, stun.Mapped());
        Rng rng(settings.seed * 2 + 2);
        const double first = now();
        uint32_t attempts = 0;
        bool waitedForTooMany = false;
        for (;;) {
            ++attempts;
            uint64_t nonce = 0;
            while (nonce == 0) nonce = static_cast<uint64_t>(rng.NextU32()) << 32 | rng.NextU32();

            RendezvousJoin join(https, settings.rendezvous, candidates, nonce, now());
            while (!join.Done()) {
                join.Update(now());
                for (const std::string& line : join.TakeLog()) Log::Info("{}", line);
                if (!join.Done()) pump();
            }
            for (const std::string& line : join.TakeLog()) Log::Info("{}", line);
            if (join.Failure() == JoinFailure::TooManyJoins && !waitedForTooMany && now() - first + 15.0 < 60.0) {
                waitedForTooMany = true;
                Log::Info("Rendezvous: too many joins to this room in a minute; joining again in 15 s");
                pumpFor(15.0);
                continue;
            }
            if (join.Failure() != JoinFailure::None || !join.Result()) {
                error = ToString(join.Failure());
                return std::nullopt;
            }

            const ClientJoinSetup& setup = *join.Result();
            Log::Info("Rendezvous: join {} posted; the server was last seen {:.0f} s ago, {} candidate(s)", join.JoinId(),
                      setup.serverAgeSeconds, setup.server.size());
            const double punchStart = now();
            traversal.emplace(setup, TraversalLimits{}, punchStart);
            while (traversal->State() == TraversalState::Punching) pump();

            if (traversal->State() == TraversalState::Reached) {
                const Candidate chosen = traversal->Chosen();
                Log::Info("Punch: join {} reached {} ({}) in {:.0f} ms", join.JoinId(), FormatEndpoint(chosen.at), ToString(chosen.kind),
                          traversal->ReachedAfter() * 1000.0);
                pumpFor(1.0);   // still answering the server's probes: it proves its side too
                return chosen.at;
            }

            const TraversalFailure failure = traversal->Failure();
            const std::string text = FailureText(failure, setup.serverAgeSeconds);
            Log::Info("Punch: join {} failed after {:.0f} s: {}", join.JoinId(), now() - punchStart, text);
            traversal.reset();
            if (failure == TraversalFailure::ServerStale && attempts < 4 && now() - first + 5.0 < 60.0) {
                pumpFor(5.0);
                continue;
            }
            error = text;
            return std::nullopt;
        }
    }
}
