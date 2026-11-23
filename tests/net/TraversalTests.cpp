// docs/design/rendezvous.md [nat-R0]: the STUN codec, SHA-256 / HMAC, the room and host keys, and hole
// punching (net::ClientTraversal / ServerTraversal / PunchTest) over NatSim - every row of "Who can
// meet", the lan choice, the proof, the caps and R0's pair test with its pastes minutes apart. No
// socket and no real STUN server: NatSim moves the datagrams on a simulated clock.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "NatSim.h"

import vortex.core;
import vortex.net;

using namespace vortex;
using natsim::Filtering;
using natsim::Mapping;
using natsim::NatSettings;
using net::Endpoint;
using net::Ipv4;

namespace {
    std::string HexOf(std::span<const uint8_t> bytes) {
        std::string out;
        for (const uint8_t b : bytes) {
            char text[3];
            std::snprintf(text, sizeof(text), "%02x", b);
            out += text;
        }
        return out;
    }

    std::vector<uint8_t> Bytes(std::string_view hex) {
        std::vector<uint8_t> out;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
        return out;
    }

    net::Token TokenOf(uint8_t first) {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(first + i);
        return t;
    }

    /// A punch datagram signed with `token` (an impostor signs with another one).
    net::Datagram PunchFrom(Endpoint from, const net::Token& roomToken, const net::Token& signer, net::PunchKind kind, net::PunchRole sender,
                            uint64_t nonceClient, uint64_t nonceServer, std::string_view clientCandidates) {
        net::Punch p;
        p.kind = kind;
        p.sender = sender;
        p.room = net::RoomTag(roomToken);
        p.nonceClient = nonceClient;
        p.nonceServer = nonceServer;
        p.proof = net::PunchProof(signer, p, net::CandHash(clientCandidates));
        const auto b = net::EncodePunch(p);
        return {from, std::vector<uint8_t>(b.begin(), b.end())};
    }

    size_t CountKind(const std::vector<net::Datagram>& out, net::PunchKind kind) {
        size_t n = 0;
        for (const net::Datagram& d : out) {
            net::Punch p;
            if (net::DecodePunch(d.bytes, p) && p.kind == kind) ++n;
        }
        return n;
    }

    // --- Two sides of the pair test over NatSim ------------------------------------------------------

    const Endpoint kStun1{Ipv4(198, 51, 100, 1), 3478};
    const Endpoint kStun2{Ipv4(198, 51, 100, 2), 3478};
    const net::Token kToken = TokenOf(0x40);

    struct PairSetup {
        NatSettings server, client;
        bool sameNat = false;           ///< both behind the server's NAT (a home, or one ISP's CGNAT)
        bool sameLan = false;           ///< ... and on one LAN (else each its own subnet)
        net::CandidateKinds kinds;
        net::TraversalLimits limits;    ///< R1's by default
        double serverPasteAt = 1.0, clientPasteAt = 1.0;
        double endAt = 20.0;
        double step = 0.005;
    };

    struct PairResult {
        net::PunchTest::Phase server = net::PunchTest::Phase::Gathering, client = net::PunchTest::Phase::Gathering;
        net::Candidate serverAt, clientAt;   ///< what each reached
        net::TraversalFailure serverFailure = net::TraversalFailure::None, clientFailure = net::TraversalFailure::None;
        net::NatClass serverClass = net::NatClass::Unknown, clientClass = net::NatClass::Unknown;
        double clientReachedAt = -1.0, serverReachedAt = -1.0;
        std::vector<std::string> log;
    };

    bool Final(net::PunchTest::Phase p) { return p == net::PunchTest::Phase::Reached || p == net::PunchTest::Phase::Failed; }

    PairResult RunPair(PairSetup s) {
        natsim::NatSim sim;
        sim.AddSocket(-1, kStun1);
        sim.AddSocket(-1, kStun2);
        if (s.server.outsideIps.empty()) s.server.outsideIps = {Ipv4(203, 0, 113, 10)};
        if (s.client.outsideIps.empty()) s.client.outsideIps = {Ipv4(203, 0, 113, 20)};
        const int serverNat = sim.AddNat(s.server);
        const int clientNat = s.sameNat ? serverNat : sim.AddNat(s.client);
        const Endpoint serverSocket{Ipv4(192, 168, 1, 10), 5000};
        const Endpoint clientSocket{s.sameLan ? Ipv4(192, 168, 1, 20) : Ipv4(192, 168, 2, 20), 6000};
        sim.AddSocket(serverNat, serverSocket);
        sim.AddSocket(clientNat, clientSocket);
        const auto settings = [&](net::PunchRole role, Endpoint socket, uint64_t seed) {
            net::PunchTestSettings p;
            p.role = role;
            p.token = kToken;
            p.stunServers = {kStun1, kStun2};
            p.interfaces = {{"eth0", socket.ip, Ipv4(255, 255, 255, 0)}};
            p.port = socket.port;
            p.kinds = s.kinds;
            p.seed = seed;
            p.limits = s.limits;
            return p;
        };
        net::PunchTest server(settings(net::PunchRole::Server, serverSocket, 11), 0.0);
        net::PunchTest client(settings(net::PunchRole::Client, clientSocket, 22), 0.0);
        PairResult r;
        bool serverPasted = false, clientPasted = false;
        std::vector<net::Datagram> out;
        for (double t = 0.0; t <= s.endAt; t += s.step) {
            sim.Step(t);
            natsim::ServeStun(sim, kStun1, t);
            natsim::ServeStun(sim, kStun2, t);
            for (const auto& [test, socket] : {std::pair{&server, serverSocket}, std::pair{&client, clientSocket}}) {
                for (const net::Datagram& d : sim.Receive(socket)) test->OnDatagram(d, t);
                out.clear();
                test->Update(t, out);
                for (net::Datagram& d : out) sim.Send(socket, d.address, std::move(d.bytes), t);
                for (std::string& line : test->TakeLog()) r.log.push_back(std::move(line));
            }
            std::string error;
            // A side takes a paste once it has printed its own line (STUN settled or gave up).
            if (!serverPasted && t >= s.serverPasteAt && !client.Line().empty() && !server.Line().empty()) {
                serverPasted = server.Paste("pasted: " + client.Line(), t, error);
                EXPECT_TRUE(serverPasted) << error;
            }
            if (!clientPasted && t >= s.clientPasteAt && !server.Line().empty() && !client.Line().empty()) {
                clientPasted = client.Paste(server.Line(), t, error);
                EXPECT_TRUE(clientPasted) << error;
            }
            if (r.clientReachedAt < 0.0 && client.CurrentPhase() == net::PunchTest::Phase::Reached) r.clientReachedAt = t;
            if (r.serverReachedAt < 0.0 && server.CurrentPhase() == net::PunchTest::Phase::Reached) r.serverReachedAt = t;
            if (Final(server.CurrentPhase()) && Final(client.CurrentPhase())) break;
        }
        r.server = server.CurrentPhase();
        r.client = client.CurrentPhase();
        r.serverAt = server.ReachedAt();
        r.clientAt = client.ReachedAt();
        r.serverFailure = server.Failure();
        r.clientFailure = client.Failure();
        r.serverClass = server.Class();
        r.clientClass = client.Class();
        return r;
    }

    std::string Describe(const PairResult& r) {
        std::string s;
        for (const std::string& line : r.log) s += line + "\n";
        return s;
    }

    NatSettings Nat(Mapping m, Filtering f) {
        NatSettings n;
        n.mapping = m;
        n.filtering = f;
        return n;
    }
}

// --- STUN ------------------------------------------------------------------------------------------

// RFC 5769 2.2: the sample IPv4 response (SOFTWARE, XOR-MAPPED-ADDRESS, MESSAGE-INTEGRITY, FINGERPRINT)
// gives 192.0.2.1:32853; a MAPPED-ADDRESS-only answer is the fallback; a fake server's answer round-trips.
TEST(Stun, ParsesTheRfc5769IPv4Response) {
    const std::vector<uint8_t> response = Bytes(
        "0101003c2112a442b7e7a701bc34d686fa87dfae"
        "8022000b7465737420766563746f7220"
        "002000080001a147e112a643"
        "000800142b91f599fd9e90c38c7489f92af9ba53f06be7d7"
        "80280004c07d4c96");
    net::StunTransaction t{};
    const std::vector<uint8_t> id = Bytes("b7e7a701bc34d686fa87dfae");
    std::copy(id.begin(), id.end(), t.begin());
    Endpoint mapped;
    ASSERT_EQ(net::ParseStunBindingResponse(response, t, mapped), net::StunResult::Ok);
    EXPECT_EQ(mapped, (Endpoint{Ipv4(192, 0, 2, 1), 32853}));

    // MAPPED-ADDRESS (0x0001), not xored: the fallback when XOR-MAPPED-ADDRESS is absent.
    std::vector<uint8_t> plain = Bytes("0101000c2112a442b7e7a701bc34d686fa87dfae" "0001000800011f90cb007107");
    ASSERT_EQ(net::ParseStunBindingResponse(plain, t, mapped), net::StunResult::Ok);
    EXPECT_EQ(mapped, (Endpoint{Ipv4(203, 0, 113, 7), 8080}));

    const auto request = net::MakeStunBindingRequest(t);
    net::StunTransaction seen{};
    ASSERT_TRUE(net::ParseStunBindingRequest(request, seen));
    EXPECT_EQ(seen, t);
    const std::vector<uint8_t> answer = net::MakeStunBindingResponse(t, {Ipv4(100, 64, 1, 2), 40001});
    ASSERT_EQ(net::ParseStunBindingResponse(answer, t, mapped), net::StunResult::Ok);
    EXPECT_EQ(mapped, (Endpoint{Ipv4(100, 64, 1, 2), 40001}));

    EXPECT_EQ(net::ClassifyNat(mapped, mapped), net::NatClass::Eim);
    EXPECT_EQ(net::ClassifyNat(mapped, Endpoint{mapped.ip, static_cast<uint16_t>(mapped.port + 1)}), net::NatClass::Symmetric);
    EXPECT_EQ(net::ClassifyNat(mapped, std::nullopt), net::NatClass::Unknown);
}

// Another transaction id, a missing cookie, an error response or a cut message are refused.
TEST(Stun, RefusesAWrongTransactionOrCookie) {
    net::StunTransaction t{};
    for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(i + 1);
    const std::vector<uint8_t> good = net::MakeStunBindingResponse(t, {Ipv4(203, 0, 113, 5), 5000});
    Endpoint mapped;
    net::StunTransaction other = t;
    other[11] ^= 1;
    EXPECT_EQ(net::ParseStunBindingResponse(good, other, mapped), net::StunResult::WrongTransaction);
    std::vector<uint8_t> noCookie = good;
    noCookie[4] ^= 0x01;
    EXPECT_EQ(net::ParseStunBindingResponse(noCookie, t, mapped), net::StunResult::NotStun);
    std::vector<uint8_t> error = good;
    error[1] = 0x11;   // Binding error response
    EXPECT_EQ(net::ParseStunBindingResponse(error, t, mapped), net::StunResult::NotSuccess);
    std::vector<uint8_t> cut(good.begin(), good.end() - 4);
    EXPECT_EQ(net::ParseStunBindingResponse(cut, t, mapped), net::StunResult::Malformed);
    EXPECT_EQ(net::ParseStunBindingResponse(std::vector<uint8_t>(good.begin(), good.begin() + 12), t, mapped), net::StunResult::NotStun);

    // A StunProbe takes only its own servers' answers to its own transactions.
    net::StunProbe probe({kStun1, kStun2}, 7);
    std::vector<net::Datagram> out;
    probe.Update(0.0, out);
    ASSERT_EQ(out.size(), 2u);
    net::StunTransaction sent{};
    ASSERT_TRUE(net::ParseStunBindingRequest(out[0].bytes, sent));
    EXPECT_FALSE(probe.OnDatagram({kStun1, net::MakeStunBindingResponse(t, {1, 1})}, 0.01));        // not its transaction
    EXPECT_FALSE(probe.OnDatagram({kStun2, net::MakeStunBindingResponse(sent, {1, 1})}, 0.01));     // not from that server
    EXPECT_TRUE(probe.OnDatagram({kStun1, net::MakeStunBindingResponse(sent, {Ipv4(203, 0, 113, 9), 7000})}, 0.01));
    EXPECT_FALSE(probe.Settled());
    // The second server never answers: 3 tries 2 s apart, then the first round is over (class unknown).
    for (double t2 = 0.1; t2 < 6.5; t2 += 0.1) probe.Update(t2, out);
    EXPECT_EQ(out.size(), 4u);   // the first two, then two retries to the silent one
    EXPECT_TRUE(probe.Settled());
    EXPECT_EQ(probe.Class(), net::NatClass::Unknown);
    EXPECT_EQ(probe.Outside(), (Endpoint{Ipv4(203, 0, 113, 9), 7000}));
    // Every 15 s a new round keeps the mapping open.
    out.clear();
    probe.Update(15.05, out);
    EXPECT_EQ(out.size(), 2u);
}

// --- SHA-256 and HMAC --------------------------------------------------------------------------------

TEST(Sha256, MatchesFips180Vectors) {
    EXPECT_EQ(HexOf(net::Sha256::Of(std::string_view(""))), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(HexOf(net::Sha256::Of(std::string_view("abc"))), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(HexOf(net::Sha256::Of(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(HexOf(net::Sha256::Of(std::string_view("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"))),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    // One million 'a', fed in uneven pieces (the buffering across blocks).
    net::Sha256 h;
    const std::string chunk(997, 'a');
    size_t left = 1000000;
    while (left > 0) {
        const size_t n = std::min(left, chunk.size());
        h.Update(std::string_view(chunk.data(), n));
        left -= n;
    }
    EXPECT_EQ(HexOf(h.Finish()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, HmacMatchesRfc4231Vectors) {
    const auto hmac = [](std::vector<uint8_t> key, std::vector<uint8_t> data) { return HexOf(net::HmacSha256(key, data)); };
    const auto text = [](std::string_view s) { return std::vector<uint8_t>(s.begin(), s.end()); };
    EXPECT_EQ(hmac(std::vector<uint8_t>(20, 0x0b), text("Hi There")), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    EXPECT_EQ(hmac(text("Jefe"), text("what do ya want for nothing?")), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    EXPECT_EQ(hmac(std::vector<uint8_t>(20, 0xaa), std::vector<uint8_t>(50, 0xdd)), "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
    EXPECT_EQ(hmac(Bytes("0102030405060708090a0b0c0d0e0f10111213141516171819"), std::vector<uint8_t>(50, 0xcd)),
              "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
    EXPECT_EQ(hmac(std::vector<uint8_t>(131, 0xaa), text("Test Using Larger Than Block-Size Key - Hash Key First")),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    EXPECT_EQ(hmac(std::vector<uint8_t>(131, 0xaa),
                   text("This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed "
                        "before being used by the HMAC algorithm.")),
              "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

// The exact inputs of the room key, the host key and the punch proof (computed independently).
TEST(Rendezvous, KeysOfAKnownToken) {
    net::Token token{};
    for (size_t i = 0; i < token.size(); ++i) token[i] = static_cast<uint8_t>(i);
    EXPECT_EQ(HexOf(net::RoomKey(token)), "94c0f469fd9e5d1ce662cc248d17ff7a");
    EXPECT_EQ(HexOf(net::HostKey(token)), "f7282e40f4707117ccb8083adbd5a27a");
    net::Token other{};
    ASSERT_TRUE(net::ParseToken("8f3c2a9d0b1e4f5a6c7d8e9fa0b1c2d3", other));
    EXPECT_EQ(HexOf(net::RoomKey(other)), "014781f2a11af964ebb9a1b4837d2eb6");
    EXPECT_EQ(HexOf(net::HostKey(other)), "4213fb1924d29d372032bf2b5b2cba20");

    // A client's first Probe: bytes 4-30 = version, kind, sender, room tag, nonces; candHash over the
    // candidate string as written.
    net::Punch p;
    p.kind = net::PunchKind::Probe;
    p.sender = net::PunchRole::Client;
    p.room = net::RoomTag(token);
    p.nonceClient = 0x0123456789abcdefull;
    p.nonceServer = 0;
    p.proof = net::PunchProof(token, p, net::CandHash("10.0.0.2:5000/lan"));
    EXPECT_EQ(HexOf(p.proof), "fd9260c2ec17aef4");
    const auto bytes = net::EncodePunch(p);
    EXPECT_EQ(bytes.size(), 39u);
    EXPECT_EQ(HexOf(std::span<const uint8_t>(bytes.data(), 7)), "5658504e010143");
    net::Punch back;
    ASSERT_TRUE(net::DecodePunch(bytes, back));
    EXPECT_EQ(back.nonceClient, p.nonceClient);
    EXPECT_EQ(back.proof, p.proof);
}

// --- Traversal over NatSim -----------------------------------------------------------------------------

// Every row of rendezvous.md "Who can meet", "no" rows included, with what each side reports, and the
// NAT class StunProbe gives each side (two STUN servers on different IPs).
TEST(Traversal, TheMeetTableHolds) {
    const NatSettings eimEi = Nat(Mapping::Independent, Filtering::Independent);
    const NatSettings eimAddr = Nat(Mapping::Independent, Filtering::Address);
    const NatSettings eimPort = Nat(Mapping::Independent, Filtering::Port);
    const NatSettings symPort = Nat(Mapping::Symmetric, Filtering::Port);
    NatSettings symPooled = Nat(Mapping::Symmetric, Filtering::Port);
    symPooled.outsideIps = {Ipv4(203, 0, 113, 20), Ipv4(203, 0, 113, 21), Ipv4(203, 0, 113, 22)};
    NatSettings blocked = eimEi;
    blocked.blockOutbound = true;

    struct Row {
        const char* name;
        NatSettings server, client;
        bool meets;
        net::CandidateKind clientKind = net::CandidateKind::Stun;   // what the client chose (meets)
        net::TraversalFailure clientFailure = net::TraversalFailure::None;
    };
    const std::vector<Row> rows = {
        {"EIM / EIM, endpoint-independent filtering", eimEi, eimEi, true},
        {"EIM / EIM, port-dependent filtering both sides", eimPort, eimPort, true},
        {"EIM address-dependent / EIM port-dependent", eimAddr, eimPort, true},
        {"EIM endpoint-independent / symmetric, one outside IP", eimEi, symPort, true},
        {"EIM address-dependent / symmetric, one outside IP", eimAddr, symPort, true},
        {"EIM endpoint-independent / symmetric, pooled IPs", eimEi, symPooled, true},
        {"EIM port-dependent / symmetric", eimPort, symPort, false, {}, net::TraversalFailure::NoAnswer},
        {"symmetric / EIM endpoint-independent", symPort, eimEi, true, net::CandidateKind::PeerReflexive},
        {"symmetric / EIM address-dependent", symPort, eimAddr, true, net::CandidateKind::PeerReflexive},
        {"symmetric / EIM port-dependent", symPort, eimPort, false, {}, net::TraversalFailure::NoAnswer},
        {"symmetric / symmetric", symPort, symPort, false, {}, net::TraversalFailure::NoAnswer},
        {"server with outbound UDP blocked", blocked, eimEi, false, {}, net::TraversalFailure::NoAnswer},
        {"client with outbound UDP blocked", eimEi, blocked, false, {}, net::TraversalFailure::NoAnswer},
        // A symmetric NAT that filters by nothing lets the server's probe in, but its answer leaves from a
        // new port the port-dependent server drops: heard, and still no meeting.
        {"EIM port-dependent / symmetric with endpoint-independent filtering", eimPort, Nat(Mapping::Symmetric, Filtering::Independent), false,
         {}, net::TraversalFailure::CannotMeet},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        PairSetup s;
        s.server = row.server;
        s.client = row.client;
        const PairResult r = RunPair(s);
        if (row.meets) {
            EXPECT_EQ(r.client, net::PunchTest::Phase::Reached) << Describe(r);
            EXPECT_EQ(r.server, net::PunchTest::Phase::Reached) << Describe(r);
            EXPECT_EQ(r.clientAt.kind, row.clientKind) << Describe(r);
            EXPECT_LT(r.clientReachedAt, 2.0) << Describe(r);   // pasted at 1 s
        } else {
            EXPECT_EQ(r.client, net::PunchTest::Phase::Failed) << Describe(r);
            EXPECT_EQ(r.server, net::PunchTest::Phase::Failed) << Describe(r);
            EXPECT_EQ(r.clientFailure, row.clientFailure) << Describe(r);
        }
        const auto expectedClass = [](const NatSettings& n) {
            return n.blockOutbound ? net::NatClass::Unknown : n.mapping == Mapping::Independent ? net::NatClass::Eim : net::NatClass::Symmetric;
        };
        EXPECT_EQ(r.serverClass, expectedClass(row.server));
        EXPECT_EQ(r.clientClass, expectedClass(row.client));
    }

    // Both behind one ISP's CGNAT (no lan candidates between them): only a NAT that hairpins.
    for (const bool hairpin : {true, false}) {
        SCOPED_TRACE(hairpin ? "one CGNAT, hairpinning" : "one CGNAT, no hairpinning");
        PairSetup s;
        s.server = eimPort;
        s.server.hairpin = hairpin;
        s.sameNat = true;
        s.kinds.lan = false;
        const PairResult r = RunPair(s);
        EXPECT_EQ(r.client, hairpin ? net::PunchTest::Phase::Reached : net::PunchTest::Phase::Failed) << Describe(r);
        EXPECT_EQ(r.server, hairpin ? net::PunchTest::Phase::Reached : net::PunchTest::Phase::Failed) << Describe(r);
        if (!hairpin) EXPECT_EQ(r.clientFailure, net::TraversalFailure::NoAnswer);
    }

    // A stale server is reported as such, not blamed on the NAT.
    net::ClientJoinSetup stale;
    stale.token = kToken;
    stale.nonce = 5;
    stale.candidates = "-";
    stale.server = {{{Ipv4(203, 0, 113, 10), 5000}, net::CandidateKind::Stun}};
    stale.serverAgeSeconds = 40.0;
    net::ClientTraversal c(stale, {}, 0.0);
    std::vector<net::Datagram> out;
    for (double t = 0.0; t <= 10.05; t += 0.05) c.Update(t, out);
    EXPECT_EQ(c.State(), net::TraversalState::Failed);
    EXPECT_EQ(c.Failure(), net::TraversalFailure::ServerStale);
}

// Two players at one home connect over the LAN, not through the NAT's hairpin (on or off); and the
// 150 ms choice: a better kind proven within it wins, one proven later does not.
TEST(Traversal, SameLanPrefersTheLanCandidate) {
    for (const bool hairpin : {true, false}) {
        SCOPED_TRACE(hairpin ? "hairpinning" : "no hairpinning");
        PairSetup s;
        s.server = Nat(Mapping::Independent, Filtering::Port);
        s.server.hairpin = hairpin;
        s.sameNat = true;
        s.sameLan = true;
        const PairResult r = RunPair(s);
        ASSERT_EQ(r.client, net::PunchTest::Phase::Reached) << Describe(r);
        ASSERT_EQ(r.server, net::PunchTest::Phase::Reached) << Describe(r);
        EXPECT_EQ(r.clientAt.kind, net::CandidateKind::Lan);
        EXPECT_EQ(r.clientAt.at, (Endpoint{Ipv4(192, 168, 1, 10), 5000}));
        EXPECT_EQ(r.serverAt.kind, net::CandidateKind::Lan);
    }

    const Endpoint lan{Ipv4(192, 168, 1, 10), 5000}, stun{Ipv4(203, 0, 113, 10), 5000};
    const uint64_t nonceS = 77;
    for (const double lanAckAt : {0.12, 0.25}) {
        SCOPED_TRACE(lanAckAt);
        net::ClientJoinSetup setup;
        setup.token = kToken;
        setup.nonce = 9;
        setup.candidates = "192.168.1.20:6000/lan";
        setup.server = {{lan, net::CandidateKind::Lan}, {stun, net::CandidateKind::Stun}};
        net::ClientTraversal c(setup, {}, 0.0);
        std::vector<net::Datagram> out;
        c.Update(0.0, out);   // probes both
        ASSERT_TRUE(c.OnDatagram(PunchFrom(stun, kToken, kToken, net::PunchKind::Ack, net::PunchRole::Server, 9, nonceS, setup.candidates), 0.05));
        for (double t = 0.06; t < lanAckAt; t += 0.01) c.Update(t, out);
        c.OnDatagram(PunchFrom(lan, kToken, kToken, net::PunchKind::Ack, net::PunchRole::Server, 9, nonceS, setup.candidates), lanAckAt);
        c.Update(lanAckAt + 0.001, out);
        ASSERT_EQ(c.State(), net::TraversalState::Reached);
        EXPECT_EQ(c.Chosen().kind, lanAckAt < 0.2 ? net::CandidateKind::Lan : net::CandidateKind::Stun);
    }
}

// Someone with the room key but not the token (Cloudflare, a reader of D1, an impostor server while
// the real one is down) cannot prove anything: its Probes and Acks are dropped, nothing is answered,
// and the client never reaches it (no Hello would go there).
TEST(Traversal, AnImpostorWithTheRoomKeyGetsNoHello) {
    const net::Token impostor = TokenOf(0x90);
    const Endpoint fake{Ipv4(203, 0, 113, 66), 5000};
    net::ClientJoinSetup setup;
    setup.token = kToken;
    setup.nonce = 0x1111;
    setup.candidates = "203.0.113.20:6000/stun";
    setup.server = {{fake, net::CandidateKind::Stun}};
    net::ClientTraversal c(setup, {}, 0.0);
    std::vector<net::Datagram> out;
    for (double t = 0.0; t <= 10.05; t += 0.01) {
        out.clear();
        c.Update(t, out);
        // It sees the room tag and the client's nonce on the wire, and signs with the token it has.
        EXPECT_FALSE(c.OnDatagram(PunchFrom(fake, kToken, impostor, net::PunchKind::Probe, net::PunchRole::Server, setup.nonce, 5, setup.candidates), t));
        EXPECT_FALSE(c.OnDatagram(PunchFrom(fake, kToken, impostor, net::PunchKind::Ack, net::PunchRole::Server, setup.nonce, 5, setup.candidates), t));
        EXPECT_EQ(CountKind(out, net::PunchKind::Ack), 0u);
    }
    EXPECT_EQ(c.State(), net::TraversalState::Failed);
    EXPECT_EQ(c.Failure(), net::TraversalFailure::NoAnswer);
    EXPECT_FALSE(c.HeardServer());

    // The server side drops an impostor client's probes the same way.
    net::ServerTraversal s(kToken, {}, {}, 3);
    ASSERT_EQ(s.AddJoin({1, 0x2222, "203.0.113.20:6000/stun", true}, 0.0), net::AddJoinResult::Added);
    EXPECT_FALSE(s.OnDatagram(PunchFrom(fake, kToken, impostor, net::PunchKind::Probe, net::PunchRole::Client, 0x2222, 0, "203.0.113.20:6000/stun"), 0.1));
    out.clear();
    s.Update(0.11, out);
    EXPECT_EQ(CountKind(out, net::PunchKind::Ack), 0u);
}

// A join that copies the client's nonce with other candidates (the Worker refuses it with 409; here it
// reached the server anyway) proves nothing to the real client: the server's proofs for it cover the
// copy's candidates. And the server refuses a second join with a client nonce it holds.
TEST(Traversal, AJoinThatCopiesAnotherJoinsNonceProvesNothingToIt) {
    const std::string realCands = "203.0.113.20:6000/stun", copyCands = "203.0.113.99:6000/stun";
    const uint64_t nonce = 0xabcdef;
    net::ServerTraversal server(kToken, {}, {}, 4);
    ASSERT_EQ(server.AddJoin({1, nonce, copyCands, true}, 0.0), net::AddJoinResult::Added);
    EXPECT_EQ(server.AddJoin({2, nonce, realCands, true}, 0.01), net::AddJoinResult::DuplicateNonce);

    net::ClientJoinSetup setup;
    setup.token = kToken;
    setup.nonce = nonce;
    setup.candidates = realCands;
    const Endpoint serverAt{Ipv4(203, 0, 113, 10), 5000};
    setup.server = {{serverAt, net::CandidateKind::Stun}};
    net::ClientTraversal client(setup, {}, 0.0);
    std::vector<net::Datagram> fromServer, fromClient;
    for (double t = 0.0; t <= 10.05; t += 0.01) {
        fromServer.clear();
        server.Update(t, fromServer);
        for (net::Datagram& d : fromServer) {
            d.address = serverAt;   // delivered to the real client as if it had gone there
            EXPECT_FALSE(client.OnDatagram(d, t));
        }
        fromClient.clear();
        client.Update(t, fromClient);
    }
    EXPECT_EQ(client.State(), net::TraversalState::Failed);
    EXPECT_FALSE(client.HeardServer());
    EXPECT_GT(server.SentFor(1), 0u);
}

// The server answers only proven, live joins: a valid Probe gets one Ack; a wrong proof, an unknown
// nonce, another server nonce, or a join older than 15 s get nothing. The client takes only its own
// nonce with a non-zero server nonce, and the same one after the first.
TEST(Traversal, AnswersOnlyProvenLiveJoins) {
    const std::string cands = "203.0.113.20:6000/stun";
    const Endpoint client{Ipv4(203, 0, 113, 20), 6000};
    net::ServerTraversal s(kToken, {}, {}, 5);
    ASSERT_EQ(s.AddJoin({1, 0x77, cands, true}, 0.0), net::AddJoinResult::Added);
    const uint64_t own = s.NonceFor(1);
    ASSERT_NE(own, 0u);
    std::vector<net::Datagram> out;
    const auto acksAfter = [&](const net::Datagram& d, double t) {
        const bool taken = s.OnDatagram(d, t);
        out.clear();
        s.Update(t, out);
        return std::pair{taken, CountKind(out, net::PunchKind::Ack)};
    };
    EXPECT_EQ(acksAfter(PunchFrom(client, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Client, 0x77, 0, cands), 0.1), std::pair(true, size_t{1}));
    EXPECT_EQ(acksAfter(PunchFrom(client, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Client, 0x77, own, cands), 0.2), std::pair(true, size_t{1}));
    EXPECT_EQ(acksAfter(PunchFrom(client, kToken, TokenOf(1), net::PunchKind::Probe, net::PunchRole::Client, 0x77, 0, cands), 0.3), std::pair(false, size_t{0}));
    EXPECT_EQ(acksAfter(PunchFrom(client, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Client, 0x78, 0, cands), 0.4), std::pair(false, size_t{0}));
    EXPECT_EQ(acksAfter(PunchFrom(client, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Client, 0x77, own + 1, cands), 0.5), std::pair(false, size_t{0}));
    EXPECT_EQ(acksAfter(PunchFrom(client, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Client, 0x77, 0, "-"), 0.6), std::pair(false, size_t{0}));
    EXPECT_EQ(acksAfter(PunchFrom(client, TokenOf(1), kToken, net::PunchKind::Probe, net::PunchRole::Client, 0x77, 0, cands), 0.7), std::pair(false, size_t{0}));
    EXPECT_EQ(acksAfter(PunchFrom(client, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Client, 0x77, 0, cands), 15.5), std::pair(false, size_t{0}));

    net::ClientJoinSetup setup;
    setup.token = kToken;
    setup.nonce = 0x99;
    setup.candidates = cands;
    const Endpoint server{Ipv4(203, 0, 113, 10), 5000};
    setup.server = {{server, net::CandidateKind::Stun}};
    net::ClientTraversal c(setup, {}, 0.0);
    EXPECT_FALSE(c.OnDatagram(PunchFrom(server, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Server, 0x99, 0, cands), 0.1));
    EXPECT_FALSE(c.OnDatagram(PunchFrom(server, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Server, 0x98, 3, cands), 0.1));
    EXPECT_FALSE(c.OnDatagram(PunchFrom(server, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Client, 0x99, 3, cands), 0.1));
    EXPECT_TRUE(c.OnDatagram(PunchFrom(server, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Server, 0x99, 3, cands), 0.1));
    EXPECT_FALSE(c.OnDatagram(PunchFrom(server, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Server, 0x99, 4, cands), 0.2));
    EXPECT_TRUE(c.OnDatagram(PunchFrom(server, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Server, 0x99, 3, cands), 0.2));
}

// At most 100 datagrams per join and side (probes and acks), at most 10 to an unverified join's stun
// candidates, lan candidates only in the server's own subnets, at most 4 joins at once.
TEST(Traversal, CapsWhatAJoinMakesEachSideSend) {
    const std::vector<net::LocalInterface> subnets = {{"eth0", Ipv4(192, 168, 1, 10), Ipv4(255, 255, 255, 0)}};
    net::ServerTraversal s(kToken, subnets, {}, 6);
    const std::string cands = "192.168.1.20:6000/lan,10.9.9.9:6000/lan,198.51.100.50:6000/lan,203.0.113.20:6000/stun,203.0.113.21:6000/stun";
    ASSERT_EQ(s.AddJoin({1, 0x10, cands, true}, 0.0), net::AddJoinResult::Added);
    ASSERT_EQ(s.AddJoin({2, 0x20, "203.0.113.30:6000/stun,203.0.113.31:6000/stun", false}, 0.0), net::AddJoinResult::Added);
    ASSERT_EQ(s.AddJoin({3, 0x30, "-", true}, 0.0), net::AddJoinResult::Added);
    ASSERT_EQ(s.AddJoin({4, 0x40, "-", true}, 0.0), net::AddJoinResult::Added);
    EXPECT_EQ(s.AddJoin({5, 0x50, "-", true}, 0.0), net::AddJoinResult::Busy);
    EXPECT_EQ(s.AddJoin({6, 0x60, "1.2.3.4:5/given", true}, 0.0), net::AddJoinResult::Busy);

    std::vector<net::Datagram> out;
    std::vector<net::Datagram> all;
    const Endpoint client{Ipv4(203, 0, 113, 20), 6000};
    for (double t = 0.0; t <= 12.0; t += 0.01) {
        // A token holder floods join 1 with valid probes: its acks count against the same cap.
        s.OnDatagram(PunchFrom(client, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Client, 0x10, 0, cands), t);
        out.clear();
        s.Update(t, out);
        all.insert(all.end(), out.begin(), out.end());
        if (t < 9.0) {
            EXPECT_LE(s.SentFor(1), 100u);
            EXPECT_LE(s.SentToStunFor(2), 10u);
        }
    }
    size_t join1 = 0, join2 = 0;
    for (const net::Datagram& d : all) {
        net::Punch p;
        ASSERT_TRUE(net::DecodePunch(d.bytes, p));
        if (p.nonceClient == 0x10) ++join1;
        if (p.nonceClient == 0x20) ++join2;
        // Never a lan candidate outside the server's subnets, nor a public "lan" one.
        EXPECT_NE(d.address.ip, Ipv4(10, 9, 9, 9));
        EXPECT_NE(d.address.ip, Ipv4(198, 51, 100, 50));
    }
    EXPECT_EQ(join1, 100u);
    EXPECT_EQ(join2, 10u);

    // The client: 8 server candidates, 10 s of rounds: still 100.
    net::ClientJoinSetup setup;
    setup.token = kToken;
    setup.nonce = 0x70;
    setup.candidates = "-";
    for (uint16_t i = 0; i < 8; ++i) setup.server.push_back({{Ipv4(203, 0, 113, 10), static_cast<uint16_t>(5000 + i)}, net::CandidateKind::Stun});
    net::ClientTraversal c(setup, {}, 0.0);
    out.clear();
    for (double t = 0.0; t <= 11.0; t += 0.01) c.Update(t, out);
    EXPECT_EQ(out.size(), 100u);
    EXPECT_EQ(c.Sent(), 100u);
}

// A Probe is answered by one Ack; an Ack is never answered (no ping-pong), on either side.
TEST(Traversal, AcksAreNeverAnswered) {
    const std::string cands = "203.0.113.20:6000/stun";
    const Endpoint clientAt{Ipv4(203, 0, 113, 20), 6000}, serverAt{Ipv4(203, 0, 113, 10), 5000};
    net::ServerTraversal s(kToken, {}, {}, 8);
    ASSERT_EQ(s.AddJoin({1, 0x33, cands, true}, 0.0), net::AddJoinResult::Added);
    const uint64_t own = s.NonceFor(1);
    net::ClientJoinSetup setup;
    setup.token = kToken;
    setup.nonce = 0x33;
    setup.candidates = cands;
    setup.server = {{serverAt, net::CandidateKind::Stun}};
    net::ClientTraversal c(setup, {}, 0.0);
    std::vector<net::Datagram> out;
    for (int i = 0; i < 5; ++i) {
        const double t = 0.01 * i;
        ASSERT_TRUE(s.OnDatagram(PunchFrom(clientAt, kToken, kToken, net::PunchKind::Ack, net::PunchRole::Client, 0x33, own, cands), t));
        ASSERT_TRUE(c.OnDatagram(PunchFrom(serverAt, kToken, kToken, net::PunchKind::Ack, net::PunchRole::Server, 0x33, own, cands), t));
        out.clear();
        s.Update(t, out);
        c.Update(t, out);
        EXPECT_EQ(CountKind(out, net::PunchKind::Ack), 0u);
    }
    ASSERT_TRUE(s.OnDatagram(PunchFrom(clientAt, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Client, 0x33, own, cands), 0.06));
    ASSERT_TRUE(c.OnDatagram(PunchFrom(serverAt, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Server, 0x33, own, cands), 0.06));
    out.clear();
    s.Update(0.06, out);
    c.Update(0.06, out);
    EXPECT_EQ(CountKind(out, net::PunchKind::Ack), 2u);
}

// R0's pair test with port-dependent filtering on both sides and the two pastes 3 minutes apart, in
// either order: STUN every 15 s keeps each mapping (60 s timeout) open while a side waits, and the side
// that pasted first keeps probing once a second, so the second paste reaches the peer at once.
TEST(Traversal, PastesMinutesApartStillMeet) {
    for (const bool serverFirst : {true, false}) {
        SCOPED_TRACE(serverFirst ? "server pasted first" : "client pasted first");
        PairSetup s;
        s.server = Nat(Mapping::Independent, Filtering::Port);
        s.client = Nat(Mapping::Independent, Filtering::Port);
        s.server.mappingTimeout = s.client.mappingTimeout = 60.0;
        s.limits = net::PairTestLimits();
        s.serverPasteAt = serverFirst ? 1.0 : 181.0;
        s.clientPasteAt = serverFirst ? 181.0 : 1.0;
        s.endAt = 200.0;
        s.step = 0.01;
        const PairResult r = RunPair(s);
        ASSERT_EQ(r.client, net::PunchTest::Phase::Reached) << Describe(r);
        ASSERT_EQ(r.server, net::PunchTest::Phase::Reached) << Describe(r);
        EXPECT_EQ(r.clientAt.kind, net::CandidateKind::Stun);
        EXPECT_EQ(r.serverAt.kind, net::CandidateKind::Stun);
        EXPECT_GT(r.clientReachedAt, 181.0);
        EXPECT_LT(r.clientReachedAt, 183.0) << Describe(r);
        EXPECT_LT(r.serverReachedAt, 183.0) << Describe(r);
    }
}

// rendezvous.md "The engine side": a lan candidate is a private (RFC 1918) address on an adapter that is not
// docker, a bridge, a VPN or a virtual machine's; and a line carries at most 4 of them, in interface
// order, so that lan never crowds given and stun out of the 8.
TEST(Traversal, LanCandidatesAreOnlyPrivateNonVirtualAddresses) {
    const uint32_t mask = Ipv4(255, 255, 255, 0);
    const auto lan = [&](const char* name, uint32_t ip) { return net::IsLanInterface({name, ip, mask}); };
    EXPECT_TRUE(lan("eth0", Ipv4(192, 168, 1, 10)));
    EXPECT_TRUE(lan("wlan0", Ipv4(10, 0, 0, 5)));
    EXPECT_TRUE(lan("enp3s0", Ipv4(172, 16, 4, 2)));
    EXPECT_TRUE(lan("Wi-Fi / Intel(R) Wi-Fi 6 AX201", Ipv4(192, 168, 0, 12)));
    EXPECT_TRUE(lan("Ethernet / Realtek PCIe GbE", Ipv4(192, 168, 0, 13)));
    EXPECT_FALSE(lan("lo", Ipv4(127, 0, 0, 1)));
    EXPECT_FALSE(lan("eth1", Ipv4(169, 254, 3, 3)));
    EXPECT_FALSE(lan("eth2", Ipv4(100, 64, 0, 1)));
    EXPECT_FALSE(lan("tailscale0", Ipv4(100, 101, 1, 1)));
    EXPECT_FALSE(lan("docker0", Ipv4(172, 17, 0, 1)));
    EXPECT_FALSE(lan("br-1a2b3c", Ipv4(172, 18, 0, 1)));
    EXPECT_FALSE(lan("virbr0", Ipv4(192, 168, 122, 1)));
    EXPECT_FALSE(lan("vboxnet0", Ipv4(192, 168, 56, 1)));
    EXPECT_FALSE(lan("vnet3", Ipv4(192, 168, 122, 7)));
    EXPECT_FALSE(lan("Ethernet 3 / TAP-Windows Adapter V9", Ipv4(10, 8, 0, 2)));
    EXPECT_FALSE(lan("vEthernet (WSL) / Hyper-V Virtual Ethernet Adapter", Ipv4(172, 20, 0, 1)));
    EXPECT_FALSE(lan("ProtonVPN / ProtonVPN Tunnel", Ipv4(10, 2, 0, 2)));
    EXPECT_FALSE(lan("eth3", Ipv4(8, 8, 8, 8)));

    const std::vector<net::LocalInterface> eth0 = {{"eth0", Ipv4(192, 168, 1, 10), mask}};
    EXPECT_TRUE(net::InSubnetOf(Ipv4(192, 168, 1, 77), eth0));
    EXPECT_FALSE(net::InSubnetOf(Ipv4(192, 168, 2, 1), eth0));
    const std::vector<net::LocalInterface> noMask = {{"eth0", Ipv4(192, 168, 1, 10), 0}};
    EXPECT_FALSE(net::InSubnetOf(Ipv4(192, 168, 1, 10), noMask));
    EXPECT_FALSE(net::InSubnetOf(Ipv4(8, 8, 8, 8), noMask));

    // The cap: six lan adapters, two STUN answers.
    net::PunchTestSettings settings;
    settings.role = net::PunchRole::Client;
    settings.token = kToken;
    settings.stunServers = {kStun1, kStun2};
    settings.port = 6000;
    for (uint8_t i = 0; i < 6; ++i)
        settings.interfaces.push_back({"eth" + std::to_string(i), Ipv4(192, 168, 1, static_cast<uint8_t>(10 + i)), mask});
    settings.seed = 3;
    net::PunchTest test(settings, 0.0);
    std::vector<net::Datagram> out;
    for (int step = 0; step <= 100; ++step) {
        const double t = 0.01 * step;
        out.clear();
        test.Update(t, out);
        for (const net::Datagram& d : out) {
            net::StunTransaction transaction{};
            if (!net::ParseStunBindingRequest(d.bytes, transaction)) continue;
            if (d.address == kStun1) test.OnDatagram({kStun1, net::MakeStunBindingResponse(transaction, {Ipv4(203, 0, 113, 20), 6000})}, t);
            if (d.address == kStun2) test.OnDatagram({kStun2, net::MakeStunBindingResponse(transaction, {Ipv4(203, 0, 113, 20), 6001})}, t);
        }
    }
    ASSERT_FALSE(test.Line().empty());
    const std::vector<net::Candidate>& c = test.Candidates();
    ASSERT_EQ(c.size(), 6u) << test.Line();
    for (uint8_t i = 0; i < 4; ++i) {
        EXPECT_EQ(c[i].kind, net::CandidateKind::Lan);
        EXPECT_EQ(c[i].at, (Endpoint{Ipv4(192, 168, 1, static_cast<uint8_t>(10 + i)), 6000}));
    }
    EXPECT_EQ(c[4].kind, net::CandidateKind::Stun);
    EXPECT_EQ(c[4].at.port, 6000);
    EXPECT_EQ(c[5].kind, net::CandidateKind::Stun);
    EXPECT_EQ(c[5].at.port, 6001);
}

// rendezvous.md "Punch datagram" (decision 1 of the [nat-R0] pack): the 10 of an unverified join count
// probes only; an Ack answers a datagram that proved the token from that address, so it is sent after
// the 10 probes too (an EIM client whose probes come from its stun address is still answered).
TEST(Traversal, AnUnverifiedJoinStillAcksAfterItsTenProbes) {
    net::ServerTraversal s(kToken, {}, {}, 9);
    const std::string cands = "203.0.113.30:6000/stun";
    const Endpoint client{Ipv4(203, 0, 113, 30), 6000};
    ASSERT_EQ(s.AddJoin({1, 0x31, cands, false}, 0.0), net::AddJoinResult::Added);
    std::vector<net::Datagram> out, all;
    for (int step = 0; step <= 39; ++step) {
        out.clear();
        s.Update(0.05 * step, out);
        all.insert(all.end(), out.begin(), out.end());
    }
    std::vector<net::Datagram> toClient;
    for (const net::Datagram& d : all)
        if (d.address == client) toClient.push_back(d);
    EXPECT_EQ(CountKind(toClient, net::PunchKind::Probe), 10u);
    for (const double t : {2.0, 2.5}) {
        SCOPED_TRACE(t);
        EXPECT_TRUE(s.OnDatagram(PunchFrom(client, kToken, kToken, net::PunchKind::Probe, net::PunchRole::Client, 0x31, 0, cands), t));
        out.clear();
        s.Update(t, out);
        EXPECT_EQ(CountKind(out, net::PunchKind::Ack), 1u);
        EXPECT_EQ(CountKind(out, net::PunchKind::Probe), 0u);
    }
    EXPECT_EQ(s.SentToStunFor(1), 10u);
}

// rendezvous.md "R0's pair test, with no Worker": a STUN server that never answers never stops the
// run. Each server is given up after 3 tries 2 s apart; then the line is printed with the lan
// candidate only and the class unknown, and the side waits for the paste.
TEST(Traversal, WithNoStunAnswerTheLineIsPrintedAndTheClassIsUnknown) {
    net::PunchTestSettings settings;
    settings.role = net::PunchRole::Server;
    settings.token = kToken;
    settings.stunServers = {kStun1, kStun2};
    settings.interfaces = {{"eth0", Ipv4(192, 168, 1, 10), Ipv4(255, 255, 255, 0)}};
    settings.port = 5000;
    settings.seed = 4;
    net::PunchTest test(settings, 0.0);
    std::vector<net::Datagram> out;
    std::vector<std::string> log;
    size_t toStun1 = 0;
    double printedAt = -1.0;
    for (int step = 0; step <= 800; ++step) {
        const double t = 0.01 * step;
        out.clear();
        test.Update(t, out);
        for (const net::Datagram& d : out)
            if (d.address == kStun1) ++toStun1;
        for (std::string& line : test.TakeLog()) log.push_back(std::move(line));
        if (step == 590) EXPECT_TRUE(test.Line().empty()) << test.Line();
        if (printedAt < 0.0 && !test.Line().empty()) printedAt = t;
    }
    ASSERT_GE(printedAt, 0.0);
    EXPECT_LE(printedAt, 7.0);
    EXPECT_EQ(test.CurrentPhase(), net::PunchTest::Phase::WaitingForPaste);
    EXPECT_EQ(test.Class(), net::NatClass::Unknown);
    EXPECT_NE(std::find(log.begin(), log.end(), "NAT: outside -, class unknown"), log.end());
    ASSERT_EQ(test.Candidates().size(), 1u);
    EXPECT_EQ(test.Candidates()[0].kind, net::CandidateKind::Lan);
    EXPECT_EQ(test.Candidates()[0].at, (Endpoint{Ipv4(192, 168, 1, 10), 5000}));
    const std::string& line = test.Line();
    EXPECT_NE(line.find("VXPT1 S "), std::string::npos) << line;
    const std::string_view tail = "192.168.1.10:5000/lan";
    ASSERT_GE(line.size(), tail.size());
    EXPECT_EQ(std::string_view(line).substr(line.size() - tail.size()), tail) << line;
    EXPECT_EQ(toStun1, 3u);   // tries at 0, 2 and 4 s; the next round starts at 15 s
}
