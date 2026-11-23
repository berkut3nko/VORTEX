// docs/design/rendezvous.md [nat-R1]: the client of the Worker - the server's room (net::Rendezvous:
// register, long poll, re-register, back off, the spent quota, delete) and one client join
// (net::RendezvousJoin) - over a fake HttpsClient. The Worker itself is tested by
// engine/tools/rendezvous/test.mjs (Tools.RendezvousWorker); its replies are those of docs/llm/nat-r1.md
// card 1 "Wire of the Worker".

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "FakeHttps.h"

import vortex.core;
import vortex.net;

using namespace vortex;
using fakehttps::FakeHttpsClient;
using net::Candidate;
using net::CandidateKind;
using net::Ipv4;

namespace {
    constexpr const char* kRoom = "94c0f469fd9e5d1ce662cc248d17ff7a";
    constexpr const char* kAuth = "Bearer f7282e40f4707117ccb8083adbd5a27a";
    constexpr const char* kQuotaLine = "Rendezvous: quota spent until 00:00 UTC";

    net::Token TokenFrom(uint8_t first) {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(first + i);
        return t;
    }

    net::RendezvousSettings Settings(uint8_t tokenFirst = 0) {
        net::RendezvousSettings s;
        s.url = "https://rv.example";
        s.token = TokenFrom(tokenFirst);
        s.buildHash = 0xab;
        s.protocol = 10;
        return s;
    }

    const std::vector<Candidate> kServer = {{{Ipv4(203, 0, 113, 10), 5000}, CandidateKind::Stun}};

    bool Has(const std::vector<std::string>& log, const std::string& line) {
        for (const std::string& l : log)
            if (l == line) return true;
        return false;
    }

    size_t Count(const std::vector<std::string>& log, const std::string& line) {
        size_t n = 0;
        for (const std::string& l : log) n += l == line;
        return n;
    }

    /// Registered (request 1 answered) and waiting (request 2 in flight) at t = 0.1.
    void Register(FakeHttpsClient& fake, net::Rendezvous& r, std::vector<std::string>& log) {
        r.Update(0.0, kServer, net::NatClass::Eim);
        fake.Respond(1, 200, "ok registered\n");
        r.Update(0.1, kServer, net::NatClass::Eim);
        for (std::string& l : r.TakeLog()) log.push_back(std::move(l));
    }
}

TEST(Rendezvous, RegistersWaitsAndDeletes) {
    FakeHttpsClient fake;
    net::Rendezvous r(fake, Settings());
    r.Update(0.0, kServer, net::NatClass::Eim);
    ASSERT_EQ(fake.requests.size(), 1u);
    const net::HttpsRequest& post = fake.At(1).request;
    EXPECT_EQ(post.method, "POST");
    EXPECT_EQ(post.url, "https://rv.example/v1/host");
    EXPECT_FALSE(post.longPoll);
    EXPECT_EQ(post.timeoutMs, 10000u);
    EXPECT_EQ(fake.Header(fake.At(1), "Authorization"), kAuth);
    EXPECT_EQ(post.body, R"({"room":"94c0f469fd9e5d1ce662cc248d17ff7a","cands":"203.0.113.10:5000/stun","nat":"eim","build":"00000000000000ab","protocol":10})");

    fake.Respond(1, 200, "ok registered\n");
    r.Update(0.1, kServer, net::NatClass::Eim);
    EXPECT_EQ(r.State(), net::RendezvousState::Waiting);
    ASSERT_EQ(fake.requests.size(), 2u);
    const net::HttpsRequest& wait = fake.At(2).request;
    EXPECT_EQ(wait.method, "GET");
    EXPECT_EQ(wait.url, std::string("https://rv.example/v1/host/wait?room=") + kRoom + "&after=0");
    EXPECT_TRUE(wait.longPoll);
    EXPECT_EQ(wait.timeoutMs, 30000u);
    EXPECT_EQ(fake.Header(fake.At(2), "Authorization"), kAuth);
    EXPECT_TRUE(Has(r.TakeLog(), "Rendezvous: room 94c0f469 registered, 1 candidates"));

    fake.Respond(2, 200, "join 7 00000000000000aa 1 203.0.113.20:6000/stun\n");
    r.Update(0.2, kServer, net::NatClass::Eim);
    const std::vector<net::ServerJoin> joins = r.TakeJoins();
    ASSERT_EQ(joins.size(), 1u);
    EXPECT_EQ(joins[0].id, 7u);
    EXPECT_EQ(joins[0].nonceClient, 0xaau);
    EXPECT_EQ(joins[0].candidates, "203.0.113.20:6000/stun");
    EXPECT_TRUE(joins[0].verified);
    EXPECT_EQ(joins[0].route, "");
    ASSERT_EQ(fake.requests.size(), 3u);
    EXPECT_EQ(fake.At(3).request.url, std::string("https://rv.example/v1/host/wait?room=") + kRoom + "&after=7");

    r.Close(0.3);
    EXPECT_TRUE(fake.At(3).cancelled);
    ASSERT_EQ(fake.requests.size(), 4u);
    const net::HttpsRequest& del = fake.At(4).request;
    EXPECT_EQ(del.method, "DELETE");
    EXPECT_EQ(del.url, "https://rv.example/v1/host");
    EXPECT_EQ(del.body, R"({"room":"94c0f469fd9e5d1ce662cc248d17ff7a"})");
    EXPECT_EQ(del.timeoutMs, 2000u);
    EXPECT_EQ(fake.Header(fake.At(4), "Authorization"), kAuth);
    EXPECT_EQ(r.State(), net::RendezvousState::Closing);

    fake.Respond(4, 200, "ok deleted\n");
    r.Update(0.4, kServer, net::NatClass::Eim);
    EXPECT_EQ(r.State(), net::RendezvousState::Closed);
    EXPECT_TRUE(Has(r.TakeLog(), "Rendezvous: room 94c0f469 deleted"));
    EXPECT_EQ(fake.requests.size(), 4u);
}

TEST(Rendezvous, ReRegistersWhenTheOutsideAddressChanges) {
    FakeHttpsClient fake;
    net::Rendezvous r(fake, Settings());
    std::vector<std::string> log;
    Register(fake, r, log);
    ASSERT_EQ(fake.requests.size(), 2u);   // the wait, in flight

    const std::vector<Candidate> moved = {{{Ipv4(203, 0, 113, 11), 5001}, CandidateKind::Stun}};
    r.Update(1.0, moved, net::NatClass::Eim);
    ASSERT_EQ(fake.requests.size(), 3u);
    EXPECT_EQ(fake.At(3).request.method, "POST");
    EXPECT_NE(fake.At(3).request.body.find(R"("cands":"203.0.113.11:5001/stun")"), std::string::npos);
    EXPECT_FALSE(fake.At(2).cancelled);
    fake.Respond(3, 200, "ok registered\n");
    r.Update(1.1, moved, net::NatClass::Eim);
    EXPECT_TRUE(Has(r.TakeLog(), "Rendezvous: registered again, 1 candidates (the candidates changed)"));

    // Only the NAT class changes.
    r.Update(2.0, moved, net::NatClass::Symmetric);
    ASSERT_EQ(fake.requests.size(), 4u);
    EXPECT_EQ(fake.At(4).request.method, "POST");
    EXPECT_NE(fake.At(4).request.body.find(R"("nat":"symmetric")"), std::string::npos);
    fake.Respond(4, 200, "ok registered\n");
    r.Update(2.1, moved, net::NatClass::Symmetric);
    EXPECT_TRUE(Has(r.TakeLog(), "Rendezvous: registered again, 1 candidates (the candidates changed)"));

    // Nothing changed: no new request (the wait of request 2 is still in flight).
    r.Update(3.0, moved, net::NatClass::Symmetric);
    r.Update(4.0, moved, net::NatClass::Symmetric);
    EXPECT_EQ(fake.requests.size(), 4u);
    EXPECT_EQ(r.State(), net::RendezvousState::Waiting);
}

TEST(Rendezvous, BacksOffAndLogsTheSpentQuotaOnce) {
    FakeHttpsClient fake;
    net::Rendezvous r(fake, Settings());
    std::vector<double> postStarts;
    std::vector<std::string> log;
    size_t answered = 0;
    double t = 0.0;
    // One server pass at t: Update, then answer every request it started with `answer`.
    const auto step = [&](const auto& answer) {
        r.Update(t, kServer, net::NatClass::Eim);
        for (; answered < fake.requests.size(); ++answered) {
            fakehttps::FakeRequest& q = fake.requests[answered];
            if (q.request.method == "POST") postStarts.push_back(t);
            answer(q);
        }
        for (std::string& l : r.TakeLog()) log.push_back(std::move(l));
        t += 0.25;
    };
    const auto refuse = [&](fakehttps::FakeRequest& q) { fake.Fail(q.id, "connection refused"); };
    const auto quota = [&](fakehttps::FakeRequest& q) { fake.Respond(q.id, 429, "error code: 1027"); };
    const auto accept = [&](fakehttps::FakeRequest& q) {
        if (q.request.method == "POST") fake.Respond(q.id, 200, "ok registered\n");
    };

    while (t <= 80.0) step(refuse);
    ASSERT_EQ(postStarts.size(), 8u);
    const double gaps[] = {1.25, 2.25, 4.25, 8.25, 16.25, 20.25, 20.25};
    for (size_t i = 0; i < 7; ++i) EXPECT_DOUBLE_EQ(postStarts[i + 1] - postStarts[i], gaps[i]) << "gap " << i;
    EXPECT_EQ(r.State(), net::RendezvousState::BackingOff);
    EXPECT_EQ(r.Failures(), 8u);
    EXPECT_TRUE(Has(log, "Rendezvous: register failed (connection refused); trying again in 1 s"));
    EXPECT_TRUE(Has(log, "Rendezvous: register failed (connection refused); trying again in 20 s"));
    EXPECT_EQ(Count(log, kQuotaLine), 0u);

    // The next three POSTs get Cloudflare's 1027 page: one quota line, nothing else logged.
    const size_t logged = log.size();
    while (postStarts.size() < 11) step(quota);
    while (r.Failures() < 11) step(quota);
    EXPECT_EQ(Count(log, kQuotaLine), 1u);
    EXPECT_EQ(log.size(), logged + 1);
    EXPECT_TRUE(r.QuotaSpent());
    EXPECT_EQ(r.State(), net::RendezvousState::BackingOff);

    // The next POST gets 200: waiting, the streak over.
    while (postStarts.size() < 12) step(accept);
    step(accept);
    EXPECT_EQ(r.State(), net::RendezvousState::Waiting);
    EXPECT_EQ(r.Failures(), 0u);
    EXPECT_FALSE(r.QuotaSpent());

    // The wait then gets 1027: a new streak, a second quota line.
    ASSERT_EQ(fake.Last()->request.method, "GET");
    fake.Respond(fake.Last()->id, 429, "error code: 1027");
    step(accept);
    EXPECT_EQ(Count(log, kQuotaLine), 2u);
    EXPECT_EQ(r.State(), net::RendezvousState::BackingOff);

    // A wait then gets 200: that streak is over too (a registered server posts nothing while its
    // candidates stand, so only the wait can end it).
    const size_t started = fake.requests.size();
    while (fake.requests.size() == started) step(accept);
    ASSERT_EQ(fake.Last()->request.method, "GET");
    EXPECT_TRUE(r.QuotaSpent());
    fake.Respond(fake.Last()->id, 200, "");
    step(accept);
    EXPECT_FALSE(r.QuotaSpent());
    EXPECT_EQ(r.Failures(), 0u);

    // The next day's quota runs out on a wait: logged again.
    ASSERT_EQ(fake.Last()->request.method, "GET");
    fake.Respond(fake.Last()->id, 429, "error code: 1027");
    step(accept);
    EXPECT_EQ(Count(log, kQuotaLine), 3u);
    EXPECT_TRUE(r.QuotaSpent());
}

TEST(Rendezvous, ARestartWithTheSameTokenKeepsTheRoom) {
    FakeHttpsClient a, b, c;
    net::Rendezvous first(a, Settings());
    net::Rendezvous second(b, Settings());
    net::Rendezvous other(c, Settings(1));
    first.Update(0.0, kServer, net::NatClass::Eim);
    second.Update(0.0, kServer, net::NatClass::Eim);
    other.Update(0.0, kServer, net::NatClass::Eim);
    ASSERT_EQ(a.requests.size(), 1u);
    ASSERT_EQ(b.requests.size(), 1u);
    ASSERT_EQ(c.requests.size(), 1u);
    EXPECT_EQ(a.At(1).request.body, b.At(1).request.body);
    EXPECT_EQ(a.Header(a.At(1), "Authorization"), b.Header(b.At(1), "Authorization"));
    EXPECT_NE(a.At(1).request.body, c.At(1).request.body);
    EXPECT_EQ(c.At(1).request.body.find(kRoom), std::string::npos);
    EXPECT_NE(a.Header(a.At(1), "Authorization"), c.Header(c.At(1), "Authorization"));
}

TEST(Rendezvous, ReRegisterAndDeleteDoNotWaitForTheLongPoll) {
    FakeHttpsClient fake;
    net::Rendezvous r(fake, Settings());
    std::vector<std::string> log;
    Register(fake, r, log);
    ASSERT_EQ(fake.requests.size(), 2u);   // the wait, never answered

    const std::vector<Candidate> moved = {{{Ipv4(203, 0, 113, 11), 5001}, CandidateKind::Stun}};
    r.Update(1.0, moved, net::NatClass::Eim);
    ASSERT_EQ(fake.requests.size(), 3u);
    EXPECT_EQ(fake.At(3).request.method, "POST");
    EXPECT_FALSE(fake.At(3).request.longPoll);
    EXPECT_FALSE(fake.At(2).cancelled);
    fake.Respond(3, 200, "ok registered\n");
    r.Update(1.1, moved, net::NatClass::Eim);
    EXPECT_TRUE(Has(r.TakeLog(), "Rendezvous: registered again, 1 candidates (the candidates changed)"));

    // The candidates move again and the server closes while that register POST is in flight: the POST is
    // cancelled, so the DELETE does not queue behind it on the short-call lane.
    const std::vector<Candidate> movedAgain = {{{Ipv4(203, 0, 113, 12), 5002}, CandidateKind::Stun}};
    r.Update(1.5, movedAgain, net::NatClass::Eim);
    ASSERT_EQ(fake.requests.size(), 4u);
    EXPECT_EQ(fake.At(4).request.method, "POST");

    r.Close(2.0);
    ASSERT_EQ(fake.requests.size(), 5u);
    EXPECT_EQ(fake.At(5).request.method, "DELETE");
    EXPECT_FALSE(fake.At(5).request.longPoll);
    EXPECT_TRUE(fake.At(2).cancelled);
    EXPECT_TRUE(fake.At(4).cancelled);
    fake.Respond(5, 200, "ok deleted\n");
    r.Update(2.1, movedAgain, net::NatClass::Eim);
    EXPECT_EQ(r.State(), net::RendezvousState::Closed);
}

TEST(Rendezvous, AnotherBuildIsRefusedBeforePunching) {
    FakeHttpsClient fake;
    const std::vector<Candidate> mine = {{{Ipv4(192, 168, 1, 20), 6000}, CandidateKind::Lan},
                                         {{Ipv4(203, 0, 113, 20), 6000}, CandidateKind::Stun}};
    net::RendezvousJoin j(fake, Settings(), mine, 0x99, 0.0);
    ASSERT_EQ(fake.requests.size(), 1u);
    const net::HttpsRequest& post = fake.At(1).request;
    EXPECT_EQ(post.method, "POST");
    EXPECT_EQ(post.url, "https://rv.example/v1/join");
    EXPECT_EQ(fake.Header(fake.At(1), "Authorization"), "");
    EXPECT_EQ(post.body,
              R"({"room":"94c0f469fd9e5d1ce662cc248d17ff7a","cands":"192.168.1.20:6000/lan,203.0.113.20:6000/stun","nonce":"0000000000000099","build":"00000000000000ab","protocol":10})");
    fake.Respond(1, 409, "error another-build 00000000000000cd\n");
    j.Update(0.1);
    EXPECT_TRUE(j.Done());
    EXPECT_EQ(j.Failure(), net::JoinFailure::AnotherBuild);
    EXPECT_FALSE(j.Result().has_value());
    EXPECT_TRUE(Has(j.TakeLog(), "Rendezvous: the server runs another build (00000000000000cd; this is 00000000000000ab)"));
    j.Update(5.0);
    EXPECT_EQ(fake.requests.size(), 1u);
}

TEST(Rendezvous, OnlyANoRoomReplyIsBlamedOnTheRoom) {
    const std::vector<Candidate> mine = {{{Ipv4(203, 0, 113, 20), 6000}, CandidateKind::Stun}};
    {
        FakeHttpsClient fake;
        net::RendezvousJoin j(fake, Settings(), mine, 0x99, 0.0);
        fake.Respond(1, 404, "error no-room\n");
        j.Update(0.1);
        ASSERT_TRUE(j.Done());
        EXPECT_EQ(j.Failure(), net::JoinFailure::NoRoom);
    }
    {
        // A wrong rendezvous.url (an extra path) reaches the Worker's unknown-path 404: not the room's fault.
        FakeHttpsClient fake;
        net::RendezvousJoin j(fake, Settings(), mine, 0x99, 0.0);
        fake.Respond(1, 404, "error not-found\n");
        j.Update(0.1);
        ASSERT_TRUE(j.Done());
        EXPECT_EQ(j.Failure(), net::JoinFailure::BadReply);
        EXPECT_TRUE(Has(j.TakeLog(), "Rendezvous: the join got HTTP 404: error not-found"));
    }
}

TEST(Rendezvous, AStaleServerIsReportedNotBlamedOnTheNat) {
    const std::vector<Candidate> mine = {{{Ipv4(203, 0, 113, 20), 6000}, CandidateKind::Stun}};
    const auto run = [&](const std::string& reply, uint32_t id, double age, net::TraversalFailure expected, const std::string& words) {
        FakeHttpsClient fake;
        net::RendezvousJoin j(fake, Settings(), mine, 0x99, 0.0);
        fake.Respond(1, 200, reply);
        j.Update(0.1);
        ASSERT_TRUE(j.Done());
        ASSERT_EQ(j.Failure(), net::JoinFailure::None);
        ASSERT_TRUE(j.Result().has_value());
        EXPECT_EQ(j.JoinId(), id);
        EXPECT_EQ(j.Result()->serverAgeSeconds, age);
        EXPECT_EQ(j.Result()->candidates, "203.0.113.20:6000/stun");
        ASSERT_EQ(j.Result()->server.size(), 1u);
        EXPECT_EQ(j.Result()->server[0], (Candidate{{Ipv4(203, 0, 113, 10), 5000}, CandidateKind::Stun}));
        net::ClientTraversal c(*j.Result(), {}, 0.0);
        std::vector<net::Datagram> out;
        for (int step = 0; step <= 101; ++step) {
            out.clear();
            c.Update(step * 0.1, out);
        }
        EXPECT_EQ(c.State(), net::TraversalState::Failed);
        EXPECT_EQ(c.Failure(), expected);
        EXPECT_EQ(net::FailureText(c.Failure(), j.Result()->serverAgeSeconds), words);
    };
    run("server 3 40 eim 203.0.113.10:5000/stun 1\n", 3, 40.0, net::TraversalFailure::ServerStale,
        "the server has not reached the rendezvous for 40 s (restarting or backing off)");
    run("server 4 5 eim 203.0.113.10:5000/stun 1\n", 4, 5.0, net::TraversalFailure::NoAnswer, 
        "no datagram from the server arrived: the server may have missed the join, or the two networks cannot meet (compare both sides' --nat-probe)");
}
