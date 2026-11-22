module;

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

module vortex.net;

import vortex.core;

namespace vortex::net {

    namespace {
        constexpr std::array<uint8_t, 4> kMagic = {'V', 'X', 'P', 'N'};
        constexpr double kNever = std::numeric_limits<double>::infinity();

        void Put64(uint8_t* p, uint64_t v) {
            for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
        }
        uint64_t Get64(const uint8_t* p) {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
            return v;
        }

        std::string Hex(std::span<const uint8_t> bytes) {
            std::string out;
            for (const uint8_t b : bytes) out += std::format("{:02x}", b);
            return out;
        }

        bool ParseHex64(std::string_view text, uint64_t& out) {
            if (text.size() != 16) return false;
            const auto r = std::from_chars(text.data(), text.data() + text.size(), out, 16);
            return r.ec == std::errc() && r.ptr == text.data() + text.size();
        }

        uint64_t NonZero64(Rng& rng) {
            for (;;) {
                const uint64_t v = static_cast<uint64_t>(rng.NextU32()) << 32 | rng.NextU32();
                if (v != 0) return v;
            }
        }

        /// The next probe round after one at `round` (`elapsed` seconds into the join).
        double NextRound(const TraversalLimits& l, double round, double elapsed, double now) {
            const double interval = elapsed < l.fastSeconds ? l.fastInterval : l.slowInterval;
            if (interval <= 0.0) return kNever;
            const double next = round + interval;
            return next > now ? next : now + interval;   // fell behind (a stall): no burst to catch up
        }

        std::vector<std::string_view> Words(std::string_view text) {
            std::vector<std::string_view> out;
            size_t i = 0;
            while (i < text.size()) {
                while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
                size_t j = i;
                while (j < text.size() && text[j] != ' ' && text[j] != '\t' && text[j] != '\r' && text[j] != '\n') ++j;
                if (j > i) out.push_back(text.substr(i, j - i));
                i = j;
            }
            return out;
        }
    }

    // --- Candidates --------------------------------------------------------------------------------

    const char* ToString(CandidateKind kind) {
        switch (kind) {
            case CandidateKind::Lan: return "lan";
            case CandidateKind::Given: return "given";
            case CandidateKind::Stun: return "stun";
            case CandidateKind::PeerReflexive: return "peer-reflexive";
        }
        return "?";
    }

    bool ParseCandidateKinds(std::string_view text, CandidateKinds& out) {
        CandidateKinds k{false, false, false};
        while (!text.empty()) {
            const size_t comma = text.find(',');
            const std::string_view word = text.substr(0, comma);
            if (word == "lan") k.lan = true;
            else if (word == "stun") k.stun = true;
            else if (word == "given") k.given = true;
            else return false;
            if (comma == std::string_view::npos) break;
            text.remove_prefix(comma + 1);
        }
        out = k;
        return true;
    }

    std::string FormatCandidates(std::span<const Candidate> candidates) {
        if (candidates.empty()) return "-";
        std::string out;
        for (const Candidate& c : candidates) {
            if (!out.empty()) out += ',';
            out += FormatEndpoint(c.at) + "/" + ToString(c.kind);
        }
        return out;
    }

    bool ParseCandidates(std::string_view text, std::vector<Candidate>& out) {
        std::vector<Candidate> list;
        if (text != "-") {
            while (!text.empty()) {
                const size_t comma = text.find(',');
                const std::string_view item = text.substr(0, comma);
                const size_t slash = item.find('/');
                if (slash == std::string_view::npos) return false;
                Candidate c;
                if (!ParseIpv4Endpoint(item.substr(0, slash), c.at)) return false;
                const std::string_view kind = item.substr(slash + 1);
                if (kind == "lan") c.kind = CandidateKind::Lan;
                else if (kind == "given") c.kind = CandidateKind::Given;
                else if (kind == "stun") c.kind = CandidateKind::Stun;
                else return false;
                list.push_back(c);
                if (list.size() > kMaxCandidates) return false;
                if (comma == std::string_view::npos) break;
                text.remove_prefix(comma + 1);
                if (text.empty()) return false;   // a trailing comma
            }
        }
        out = std::move(list);
        return true;
    }

    // --- Keys and the punch datagram ----------------------------------------------------------------

    Key16 RoomKey(const Token& token) {
        Sha256 h;
        h.Update(std::string_view("vortex-room-v1"));
        h.Update(token);
        const Sha256Digest d = h.Finish();
        Key16 k{};
        std::copy_n(d.begin(), k.size(), k.begin());
        return k;
    }

    Key16 HostKey(const Token& token) {
        const std::string_view label = "vortex-host-v1";
        const Sha256Digest d = HmacSha256(token, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(label.data()), label.size()));
        Key16 k{};
        std::copy_n(d.begin(), k.size(), k.begin());
        return k;
    }

    Tag8 CandHash(std::string_view clientCandidates) {
        const Sha256Digest d = Sha256::Of(clientCandidates);
        Tag8 t{};
        std::copy_n(d.begin(), t.size(), t.begin());
        return t;
    }

    Tag8 RoomTag(const Token& token) {
        const Key16 room = RoomKey(token);
        Tag8 t{};
        std::copy_n(room.begin(), t.size(), t.begin());
        return t;
    }

    std::array<uint8_t, kPunchBytes> EncodePunch(const Punch& p) {
        std::array<uint8_t, kPunchBytes> b{};
        std::copy(kMagic.begin(), kMagic.end(), b.begin());
        b[4] = kPunchVersion;
        b[5] = static_cast<uint8_t>(p.kind);
        b[6] = static_cast<uint8_t>(p.sender);
        std::copy(p.room.begin(), p.room.end(), b.begin() + 7);
        Put64(b.data() + 15, p.nonceClient);
        Put64(b.data() + 23, p.nonceServer);
        std::copy(p.proof.begin(), p.proof.end(), b.begin() + 31);
        return b;
    }

    Tag8 PunchProof(const Token& token, const Punch& p, const Tag8& candHash) {
        const std::array<uint8_t, kPunchBytes> b = EncodePunch(p);
        const std::string_view label = "vxpn1";
        const std::span<const uint8_t> parts[] = {
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(label.data()), label.size()),
            std::span<const uint8_t>(b.data() + 4, 27),   // bytes 4-30
            std::span<const uint8_t>(candHash),
        };
        const Sha256Digest d = HmacSha256(token, parts);
        Tag8 t{};
        std::copy_n(d.begin(), t.size(), t.begin());
        return t;
    }

    bool HasPunchMagic(std::span<const uint8_t> bytes) { return bytes.size() >= kMagic.size() && std::equal(kMagic.begin(), kMagic.end(), bytes.begin()); }

    bool DecodePunch(std::span<const uint8_t> b, Punch& out) {
        if (b.size() != kPunchBytes || !HasPunchMagic(b) || b[4] != kPunchVersion) return false;
        if (b[5] != static_cast<uint8_t>(PunchKind::Probe) && b[5] != static_cast<uint8_t>(PunchKind::Ack)) return false;
        if (b[6] != static_cast<uint8_t>(PunchRole::Server) && b[6] != static_cast<uint8_t>(PunchRole::Client)) return false;
        Punch p;
        p.kind = static_cast<PunchKind>(b[5]);
        p.sender = static_cast<PunchRole>(b[6]);
        std::copy_n(b.begin() + 7, 8, p.room.begin());
        p.nonceClient = Get64(b.data() + 15);
        p.nonceServer = Get64(b.data() + 23);
        std::copy_n(b.begin() + 31, 8, p.proof.begin());
        out = p;
        return true;
    }

    TraversalLimits PairTestLimits() {
        TraversalLimits l;
        l.fastSeconds = 10.0;
        l.fastInterval = 0.1;
        l.slowInterval = 1.0;
        l.windowSeconds = 300.0;
        l.acceptSeconds = 305.0;
        // 8 candidates x (100 fast + 290 slow rounds), and the answers: the owner's own two machines.
        l.maxDatagrams = 4000;
        l.maxJoins = 1;
        return l;
    }

    const char* ToString(TraversalFailure f) {
        switch (f) {
            case TraversalFailure::None: return "none";
            case TraversalFailure::ServerStale: return "the server has not reached the rendezvous lately (restarting or backing off)";
            case TraversalFailure::NoAnswer: return "the server did not answer the join; try again";
            case TraversalFailure::CannotMeet: return "the two networks cannot meet (see the NAT probe)";
        }
        return "?";
    }

    // --- ClientTraversal ---------------------------------------------------------------------------

    ClientTraversal::ClientTraversal(ClientJoinSetup setup, TraversalLimits limits, double now)
        : m_Setup(std::move(setup)), m_Limits(limits), m_Start(now), m_NextRound(now) {
        m_Room = RoomTag(m_Setup.token);
        m_CandHash = CandHash(m_Setup.candidates);
        for (const Candidate& c : m_Setup.server) {
            if (c.kind == CandidateKind::PeerReflexive || Find(c.at) != nullptr) continue;
            m_Targets.push_back({c, false});
            if (m_Targets.size() >= kMaxCandidates) break;
        }
    }

    ClientTraversal::Target* ClientTraversal::Find(Endpoint e) {
        for (Target& t : m_Targets)
            if (t.candidate.at == e) return &t;
        return nullptr;
    }

    void ClientTraversal::Send(Endpoint to, PunchKind kind, std::vector<Datagram>& out) {
        if (m_Sent >= m_Limits.maxDatagrams) return;
        Punch p;
        p.kind = kind;
        p.sender = PunchRole::Client;
        p.room = m_Room;
        p.nonceClient = m_Setup.nonce;
        p.nonceServer = m_NonceServer;
        p.proof = PunchProof(m_Setup.token, p, m_CandHash);
        const auto bytes = EncodePunch(p);
        out.push_back({to, std::vector<uint8_t>(bytes.begin(), bytes.end())});
        ++m_Sent;
        if (kind == PunchKind::Probe)
            if (Target* t = Find(to)) t->probed = true;
    }

    bool ClientTraversal::OnDatagram(const Datagram& in, double now) {
        Punch p;
        if (!DecodePunch(in.bytes, p) || p.sender != PunchRole::Server || p.room != m_Room || p.nonceClient != m_Setup.nonce ||
            p.nonceServer == 0)
            return false;
        if (m_NonceServer != 0 && p.nonceServer != m_NonceServer) return false;
        if (PunchProof(m_Setup.token, p, m_CandHash) != p.proof) return false;
        if (m_State == TraversalState::Failed) return true;
        m_NonceServer = p.nonceServer;
        m_Heard = true;
        Target* t = Find(in.address);
        if (t == nullptr && m_Learned < m_Limits.maxLearned) {
            // Peer-reflexive (ICE): the server is reachable at an address it did not list.
            m_Targets.push_back({{in.address, CandidateKind::PeerReflexive}, false});
            ++m_Learned;
            t = &m_Targets.back();
            if (m_State == TraversalState::Punching) m_Probes.push_back(in.address);   // probe it at once
        }
        if (p.kind == PunchKind::Probe) {
            m_Acks.push_back(in.address);
        } else if (t != nullptr && t->probed && m_State == TraversalState::Punching) {
            // An Ack from an address this side probed: proven both ways.
            if (!m_Best) {
                m_Best = t->candidate;
                m_FirstProven = now;
            } else if (t->candidate.kind < m_Best->kind) {
                m_Best = t->candidate;
            }
        }
        return true;
    }

    void ClientTraversal::Update(double now, std::vector<Datagram>& out) {
        for (const Endpoint& e : m_Acks) Send(e, PunchKind::Ack, out);
        m_Acks.clear();
        if (m_State != TraversalState::Punching) {
            m_Probes.clear();
            return;
        }
        const double elapsed = now - m_Start;
        const auto reach = [&] {
            m_State = TraversalState::Reached;
            m_Chosen = *m_Best;
            m_ReachedAfter = elapsed;
        };
        if (m_Best && (m_Best->kind == CandidateKind::Lan || now - m_FirstProven >= m_Limits.chooseSeconds)) {
            reach();
            return;
        }
        if (elapsed >= m_Limits.windowSeconds) {
            if (m_Best) {
                reach();
                return;
            }
            m_State = TraversalState::Failed;
            if (!m_Heard)
                m_Failure = m_Setup.serverAgeSeconds > m_Limits.staleServerSeconds ? TraversalFailure::ServerStale : TraversalFailure::NoAnswer;
            else
                m_Failure = TraversalFailure::CannotMeet;
            return;
        }
        for (const Endpoint& e : m_Probes) Send(e, PunchKind::Probe, out);
        m_Probes.clear();
        if (now >= m_NextRound) {
            for (Target& t : m_Targets) Send(t.candidate.at, PunchKind::Probe, out);
            m_NextRound = NextRound(m_Limits, m_NextRound, elapsed, now);
        }
    }

    // --- ServerTraversal ---------------------------------------------------------------------------

    ServerTraversal::ServerTraversal(const Token& token, std::vector<LocalInterface> subnets, TraversalLimits limits, uint64_t seed)
        : m_Token(token), m_Room(RoomTag(token)), m_Subnets(std::move(subnets)), m_Limits(limits), m_Rng(seed) {}

    ServerTraversal::Target* ServerTraversal::Find(Join& j, Endpoint e) {
        for (Target& t : j.targets)
            if (t.candidate.at == e) return &t;
        return nullptr;
    }

    AddJoinResult ServerTraversal::AddJoin(const ServerJoin& join, double now) {
        for (const Join& j : m_Joins)
            if (j.join.nonceClient == join.nonceClient) return AddJoinResult::DuplicateNonce;
        if (m_Joins.size() >= m_Limits.maxJoins) return AddJoinResult::Busy;
        std::vector<Candidate> cands;
        if (join.nonceClient == 0 || !ParseCandidates(join.candidates, cands)) return AddJoinResult::BadCandidates;
        Join j;
        j.join = join;
        j.candHash = CandHash(join.candidates);
        j.nonceServer = NonZero64(m_Rng);
        j.start = now;
        j.nextRound = now;
        size_t stun = 0;
        for (const Candidate& c : cands) {
            if (Find(j, c.at) != nullptr) continue;
            // Only lan and stun candidates are a client's; lan ones only in this server's own subnets
            // (a forged join cannot aim the server at a stranger).
            if (c.kind == CandidateKind::Lan) {
                if (!IsPrivateIpv4(c.at.ip) || !InSubnetOf(c.at.ip, m_Subnets)) continue;
            } else if (c.kind == CandidateKind::Stun) {
                if (++stun > kMaxJoinStunCandidates) continue;
            } else {
                continue;
            }
            j.targets.push_back({c, false});
        }
        m_Joins.push_back(std::move(j));
        return AddJoinResult::Added;
    }

    bool ServerTraversal::Send(Join& j, Endpoint to, PunchKind kind, std::vector<Datagram>& out) {
        if (j.sent >= m_Limits.maxDatagrams) return false;
        Target* t = Find(j, to);
        const bool toStun = t != nullptr && t->candidate.kind == CandidateKind::Stun;
        if (kind == PunchKind::Probe && toStun && !j.join.verified && j.sentToStun >= m_Limits.maxUnverifiedStun) return false;
        Punch p;
        p.kind = kind;
        p.sender = PunchRole::Server;
        p.room = m_Room;
        p.nonceClient = j.join.nonceClient;
        p.nonceServer = j.nonceServer;
        p.proof = PunchProof(m_Token, p, j.candHash);
        const auto bytes = EncodePunch(p);
        out.push_back({to, std::vector<uint8_t>(bytes.begin(), bytes.end())});
        ++j.sent;
        if (toStun) ++j.sentToStun;
        if (kind == PunchKind::Probe && t != nullptr) t->probed = true;
        return true;
    }

    bool ServerTraversal::OnDatagram(const Datagram& in, double now) {
        Punch p;
        if (!DecodePunch(in.bytes, p) || p.sender != PunchRole::Client || p.room != m_Room) return false;
        Join* found = nullptr;
        for (Join& j : m_Joins)
            if (j.join.nonceClient == p.nonceClient) found = &j;
        if (found == nullptr) return false;
        Join& j = *found;
        if (now - j.start > m_Limits.acceptSeconds) return false;
        if (p.nonceServer != 0 && p.nonceServer != j.nonceServer) return false;
        if (PunchProof(m_Token, p, j.candHash) != p.proof) return false;
        j.heard = true;
        Target* t = Find(j, in.address);
        if (t == nullptr && j.learned < m_Limits.maxLearned) {
            j.targets.push_back({{in.address, CandidateKind::PeerReflexive}, false});
            ++j.learned;
            t = &j.targets.back();
        }
        if (p.kind == PunchKind::Probe) {
            j.acks.push_back(in.address);
        } else if (p.nonceServer == j.nonceServer && t != nullptr && t->probed && !j.done) {
            if (!j.best) {
                j.best = t->candidate;
                j.firstProven = now;
            } else if (t->candidate.kind < j.best->kind) {
                j.best = t->candidate;
            }
        }
        return true;
    }

    void ServerTraversal::Update(double now, std::vector<Datagram>& out) {
        for (Join& j : m_Joins) {
            for (const Endpoint& e : j.acks) Send(j, e, PunchKind::Ack, out);
            j.acks.clear();
            if (j.done) continue;
            const double elapsed = now - j.start;
            const auto finish = [&](bool reached) {
                j.done = true;
                ServerJoinOutcome o;
                o.id = j.join.id;
                o.reached = reached;
                if (reached) o.at = *j.best;
                o.seconds = elapsed;
                o.heard = j.heard;
                m_Outcomes.push_back(o);
            };
            if (j.best && (j.best->kind == CandidateKind::Lan || now - j.firstProven >= m_Limits.chooseSeconds)) {
                finish(true);
                continue;
            }
            if (elapsed >= m_Limits.windowSeconds) {
                finish(j.best.has_value());
                continue;
            }
            // A learned address is probed at once (it may be the only way through), the rest by rounds.
            for (Target& t : j.targets)
                if (t.candidate.kind == CandidateKind::PeerReflexive && !t.probed) Send(j, t.candidate.at, PunchKind::Probe, out);
            if (now >= j.nextRound) {
                for (Target& t : j.targets) Send(j, t.candidate.at, PunchKind::Probe, out);
                j.nextRound = NextRound(m_Limits, j.nextRound, elapsed, now);
            }
        }
        // A join is dropped once it is over and its datagrams are no longer taken.
        std::erase_if(m_Joins, [&](const Join& j) { return j.done && now - j.start > m_Limits.acceptSeconds; });
    }

    std::vector<ServerJoinOutcome> ServerTraversal::TakeOutcomes() {
        std::vector<ServerJoinOutcome> out;
        out.swap(m_Outcomes);
        return out;
    }

    uint32_t ServerTraversal::SentFor(uint32_t id) const {
        for (const Join& j : m_Joins)
            if (j.join.id == id) return j.sent;
        return 0;
    }

    uint32_t ServerTraversal::SentToStunFor(uint32_t id) const {
        for (const Join& j : m_Joins)
            if (j.join.id == id) return j.sentToStun;
        return 0;
    }

    uint64_t ServerTraversal::NonceFor(uint32_t id) const {
        for (const Join& j : m_Joins)
            if (j.join.id == id) return j.nonceServer;
        return 0;
    }

    // --- PunchTest ---------------------------------------------------------------------------------

    PunchTest::PunchTest(PunchTestSettings settings, double now)
        : m_Settings(std::move(settings)), m_Stun(m_Settings.stunServers, m_Settings.seed * 2 + 1, m_Settings.stun), m_Start(now) {
        Rng rng(m_Settings.seed * 2 + 2);
        m_Nonce = NonZero64(rng);
    }

    void PunchTest::MakeLine() {
        m_Candidates.clear();
        const auto add = [&](Candidate c) {
            if (m_Candidates.size() >= kMaxCandidates) return;
            for (const Candidate& o : m_Candidates)
                if (o.at == c.at) return;
            m_Candidates.push_back(c);
        };
        if (m_Settings.kinds.lan)
            for (const LocalInterface& i : m_Settings.interfaces)
                if (IsLanInterface(i)) add({{i.ip, m_Settings.port}, CandidateKind::Lan});
        if (m_Settings.kinds.given && m_Settings.given && m_Settings.role == PunchRole::Server) add({*m_Settings.given, CandidateKind::Given});
        if (m_Settings.kinds.stun) {
            size_t stun = 0;
            for (const std::optional<Endpoint>& m : m_Stun.Mapped()) {
                if (!m || stun >= kMaxJoinStunCandidates) continue;
                const size_t before = m_Candidates.size();
                add({*m, CandidateKind::Stun});
                stun += m_Candidates.size() - before;
            }
        }
        m_CandidateText = FormatCandidates(m_Candidates);
        const std::string room = Hex(RoomTag(m_Settings.token));
        if (m_Settings.role == PunchRole::Server)
            m_Line = std::format("{} S {} {}", kPunchLinePrefix, room, m_CandidateText);
        else
            m_Line = std::format("{} C {} {:016x} {}", kPunchLinePrefix, room, m_Nonce, m_CandidateText);
    }

    bool PunchTest::OnDatagram(const Datagram& in, double now) {
        if (m_Stun.OnDatagram(in, now)) return true;
        if (!HasPunchMagic(in.bytes)) return false;
        if (m_Client) m_Client->OnDatagram(in, now);
        if (m_Server) m_Server->OnDatagram(in, now);
        return true;   // ours or nobody's: never ENet's
    }

    bool PunchTest::Paste(std::string_view text, double now, std::string& error) {
        const size_t at = text.find(kPunchLinePrefix);
        if (at == std::string_view::npos) {
            error = std::format("no {} line in it", kPunchLinePrefix);
            return false;
        }
        const std::vector<std::string_view> w = Words(text.substr(at));
        const bool fromClient = w.size() >= 2 && w[1] == "C";
        const bool fromServer = w.size() >= 2 && w[1] == "S";
        if (!fromClient && !fromServer) {
            error = "the line names no side (S or C)";
            return false;
        }
        if ((m_Settings.role == PunchRole::Server) == fromServer) {
            error = "that line is this side's own kind: paste the other program's line";
            return false;
        }
        if (m_Phase == Phase::Gathering) {
            error = "wait until this side has printed its own line";
            return false;
        }
        if (m_Phase != Phase::WaitingForPaste) {
            error = "a line was pasted already (start both programs again for another try)";
            return false;
        }
        const size_t expected = fromClient ? 5 : 4;
        if (w.size() < expected) {
            error = "the line is cut short";
            return false;
        }
        if (w[2] != Hex(RoomTag(m_Settings.token))) {
            error = "that line was made with another --token";
            return false;
        }
        std::vector<Candidate> cands;
        const std::string_view candText = w[expected - 1];
        if (!ParseCandidates(candText, cands)) {
            error = "its candidates are malformed";
            return false;
        }
        m_PastedAt = now;
        if (fromClient) {
            uint64_t nonce = 0;
            if (!ParseHex64(w[3], nonce) || nonce == 0) {
                error = "its nonce is malformed";
                return false;
            }
            std::vector<LocalInterface> subnets;
            for (const LocalInterface& i : m_Settings.interfaces)
                if (IsLanInterface(i)) subnets.push_back(i);
            m_Server.emplace(m_Settings.token, std::move(subnets), m_Settings.limits, m_Settings.seed * 2 + 3);
            ServerJoin join;
            join.id = 1;
            join.nonceClient = nonce;
            join.candidates = std::string(candText);
            join.verified = true;   // pasted by the owner
            if (m_Server->AddJoin(join, now) != AddJoinResult::Added) {
                m_Server.reset();
                error = "the join was refused";
                return false;
            }
        } else {
            ClientJoinSetup setup;
            setup.token = m_Settings.token;
            setup.nonce = m_Nonce;
            setup.candidates = m_CandidateText;
            setup.server = cands;
            m_Client.emplace(std::move(setup), m_Settings.limits, now);
        }
        m_Phase = Phase::Punching;
        Log(std::format("Punch: pasted the {}'s line ({} candidate(s): {}); punching for up to {:.0f} s", fromClient ? "client" : "server",
                        cands.size(), candText, m_Settings.limits.windowSeconds));
        return true;
    }

    void PunchTest::Update(double now, std::vector<Datagram>& out) {
        m_Stun.Update(now, out);
        if (m_Phase == Phase::Gathering && (m_Stun.Settled() || now - m_Start >= m_Settings.gatherSeconds)) {
            const std::optional<Endpoint> outside = m_Stun.Outside();
            Log(std::format("NAT: outside {}, class {}", outside ? FormatEndpoint(*outside) : std::string("-"), ToString(m_Stun.Class())));
            for (size_t i = 0; i < m_Stun.Servers().size(); ++i) {
                const std::optional<Endpoint>& m = m_Stun.Mapped()[i];
                Log(std::format("NAT: STUN server {} answered {}", FormatEndpoint(m_Stun.Servers()[i]), m ? FormatEndpoint(*m) : std::string("nothing")));
            }
            MakeLine();
            m_Phase = Phase::WaitingForPaste;
        }
        if (m_Stun.TakeChange()) {
            const std::optional<Endpoint> outside = m_Stun.Outside();
            Log(std::format("NAT: the outside address changed to {}: a line printed before is out of date (start both again)",
                            outside ? FormatEndpoint(*outside) : std::string("-")));
        }
        const double ms = (now - m_PastedAt) * 1000.0;
        if (m_Client) {
            m_Client->Update(now, out);
            if (m_Phase == Phase::Punching && m_Client->State() == TraversalState::Reached) {
                m_Phase = Phase::Reached;
                m_Reached = m_Client->Chosen();
                Log(std::format("Punch: join 1 reached {} ({}) in {:.0f} ms", FormatEndpoint(m_Reached.at), ToString(m_Reached.kind), ms));
            } else if (m_Phase == Phase::Punching && m_Client->State() == TraversalState::Failed) {
                m_Phase = Phase::Failed;
                m_Failure = m_Client->Failure();
                Log(std::format("Punch: join 1 failed after {:.0f} s: {}", now - m_PastedAt,
                                m_Failure == TraversalFailure::NoAnswer ? "no datagram from the server arrived" : ToString(m_Failure)));
            }
        }
        if (m_Server) {
            m_Server->Update(now, out);
            for (const ServerJoinOutcome& o : m_Server->TakeOutcomes()) {
                if (o.reached) {
                    m_Phase = Phase::Reached;
                    m_Reached = o.at;
                    Log(std::format("Punch: join {} reached {} ({}) in {:.0f} ms", o.id, FormatEndpoint(o.at.at), ToString(o.at.kind), o.seconds * 1000.0));
                } else {
                    m_Phase = Phase::Failed;
                    m_Failure = o.heard ? TraversalFailure::CannotMeet : TraversalFailure::NoAnswer;
                    Log(std::format("Punch: join {} failed after {:.0f} s: {}", o.id, o.seconds,
                                    o.heard ? "the client was heard, but the two networks cannot meet" : "no datagram from the client arrived"));
                }
            }
        }
    }

    std::vector<std::string> PunchTest::TakeLog() {
        std::vector<std::string> out;
        out.swap(m_Log);
        return out;
    }
}
