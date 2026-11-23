// docs/design/networking.md "Stage 8": the shared bot (net::Bot, DrainWatch, JudgeBot, the exit lines),
// the server's timeline and the latency drawn from it, and the UDP relay below ENet that impairs real
// traffic for vortex_bot.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.physics;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    net::Token RigToken() {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(0x51 + i * 3);
        return t;
    }

    /// 7 x 7 chunks of play, flat ground at 12.5 m (100 voxels: the chunk row 3 holds the surface, Dense).
    sim::RegionDesc RigRegion() {
        sim::RegionDesc r;
        r.templateId = sim::RegionTemplateId::Empty;
        r.seed = 777;
        r.playMinChunk = {-4, -4};
        r.playMaxChunk = {3, 3};
        r.heightRange = {0.0f, 32.0f};
        r.generator = sim::RegionGenerator::Flat;
        r.flatGroundVoxels = 100;
        r.terrain.voxelSize = 0.125f;
        voxel::MaterialLibrary scratch;
        r.terrain.materials = voxel::RegisterDefaultWorldMaterials(scratch);
        return r;
    }

    net::ServerSettings RigSettings() {
        net::ServerSettings s;
        s.session.token = RigToken();
        s.session.region = RigRegion();
        s.session.packHidden = false;
        s.simulation.jobThreads = 2;
        s.simulation.physics.threads = 1;
        s.simulation.physics.maxBodies = 2048;
        s.simulation.physics.maxBodyPairs = 4096;
        s.simulation.physics.maxContactConstraints = 4096;
        return s;
    }

    /// A server and net::Bots on loopback transports, one manual clock (a tick per Step), reproducible
    /// (collider cooks inside the tick).
    struct BotRig {
        struct Member {
            net::LoopbackTransport loop;
            std::unique_ptr<net::Bot> bot;
            uint32_t joinTick = 0;
            bool acting = true;
        };
        BotRig() : server(RigSettings(), serverLoop) { started = server.Start(); }

        Member& Add(net::BotSettings s, uint32_t joinTick) {
            s.client.token = RigToken();
            s.chunkCapacity = 4096;
            auto m = std::make_unique<Member>();
            m->joinTick = joinTick;
            members.push_back(std::move(m));
            Member& r = *members.back();
            r.bot = std::make_unique<net::Bot>(s, r.loop);
            return r;
        }

        void Step(bool acting = true) {
            const uint32_t tick = server.TickNumber();
            for (auto& m : members)
                if (tick == m->joinTick) net::LoopbackTransport::Connect(serverLoop, m->loop);
            server.Tick();
            server.Simulation().Jobs().WaitIdle();
            for (auto& m : members) {
                if (tick < m->joinTick) continue;
                m->bot->Pass(1.0 / 60.0, acting && m->acting);
                if (sim::Simulation* sim = m->bot->GetSimulation()) sim->Jobs().WaitIdle();
                m->bot->RecordApplied(static_cast<double>(server.TickNumber()) / 60.0);
            }
        }

        net::LoopbackTransport serverLoop;
        net::Server server;
        bool started = false;
        std::vector<std::unique_ptr<Member>> members;
    };

    /// "count resting idhash resthash" of an entities line (the fields a script compares).
    std::string EntityFields(const std::string& line) {
        std::smatch m;
        static const std::regex re(R"(([0-9]+) object\(s\), ([0-9]+) resting, id hash ([0-9a-f]{16}), rest hash ([0-9a-f]{16}))");
        return std::regex_search(line, m, re) ? m.str(1) + " " + m.str(2) + " " + m.str(3) + " " + m.str(4) : std::string("none");
    }
}

// Two shared bots (and two with a fault declared that is not there) over the loopback: they join, play,
// drain by DrainWatch, and their hashes over their sync boxes and their entity lines equal the server's.
// A bot with an injected broken chunk stands until its chunk was found and repaired, then plays. JudgeBot
// passes the honest bots and fails each wrong one with its reason; the exit lines keep the formats
// tools/session-check.sh and tools/soak.sh parse.
TEST(Bot, TwoSharedBotsJoinPlayAndDrain) {
    BotRig rig;
    ASSERT_TRUE(rig.started);
    glm::ivec3 broken{0};
    std::string why;
    ASSERT_TRUE(net::BrokenChunkCandidate(rig.server, broken, &why, -100.0)) << why;   // flat ground: no burial asked
    EXPECT_FALSE(net::BrokenChunkCandidate(rig.server, broken, nullptr, 3.0));        // nothing Dense 3 m under flat ground
    ASSERT_TRUE(net::BrokenChunkCandidate(rig.server, broken, &why, -100.0));

    net::BotSettings a;
    a.id = 0;
    a.client.presentation = true;
    a.pilot.crateEveryTicks = 200;            // crates, no casts: the broken chunk stays pristine until B's repair
    BotRig::Member& ma = rig.Add(a, 0);

    net::BotSettings b;
    b.id = 1;
    b.hasBrokenChunk = true;                  // the Bot wraps its generator itself (vortex_bot's way)
    b.brokenChunk = broken;
    b.pilot.casts = true;
    b.pilot.castEveryTicks = 90;
    BotRig::Member& mb = rig.Add(b, 240);

    // Faults declared that are not there: JudgeBot must fail them.
    net::BotSettings c;
    c.id = 2;
    c.brokenGenerator = true;
    c.client.wrapGenerator = [](std::shared_ptr<voxel::IWorldGenerator> g) { return g; };   // not broken after all
    c.stand = true;
    BotRig::Member& mc = rig.Add(c, 240);
    net::BotSettings e;
    e.id = 3;
    e.hasBrokenChunk = true;
    e.brokenChunk = broken;
    e.client.wrapGenerator = [](std::shared_ptr<voxel::IWorldGenerator> g) { return g; };
    BotRig::Member& me = rig.Add(e, 240);

    bool bStoodWhileBroken = true;
    uint64_t wantedWhileBroken = 0;
    for (uint32_t t = 0; t < 1500; ++t) {
        rig.Step();
        if (mb.bot->WaitingForRepair() && mb.bot->GetClient().Predicting()) {
            bStoodWhileBroken = bStoodWhileBroken && mb.bot->Pilot()->Casts() == 0;
            wantedWhileBroken = std::max(wantedWhileBroken, mb.bot->WantedTicks());
        }
    }
    EXPECT_TRUE(bStoodWhileBroken);
    // Recheck of stage 8: the wait for the repair is not asked of its pilot (BotGates::minActedShare).
    EXPECT_EQ(wantedWhileBroken, 0u);
    EXPECT_FALSE(mb.bot->WaitingForRepair());
    EXPECT_GT(mb.bot->Pilot()->Casts(), 0u);  // it played after its repair
    EXPECT_GT(ma.bot->Pilot()->Crates(), 0u);

    // Drain: no controls, every bot's DrainWatch holds for 1 s of the manual clock.
    std::vector<net::DrainWatch> watches(rig.members.size(), net::DrainWatch(1.0));
    bool drained = false;
    for (uint32_t t = 0; t < 1200 && !drained; ++t) {
        rig.Step(false);
        drained = true;
        for (size_t i = 0; i < rig.members.size(); ++i)
            drained = watches[i].Update(rig.members[i]->bot->GetClient(), static_cast<double>(t) / 60.0) && drained;
    }
    ASSERT_TRUE(drained);
    const std::string serverEntities = net::ServerEntitiesLine(rig.server);
    for (auto& m : rig.members) {
        const net::Client& client = m->bot->GetClient();
        EXPECT_EQ(client.AppliedOpSeq(), rig.server.LastOpSeq());
        const voxel::WorldHashResult h = client.Hash();
        EXPECT_TRUE(h.Complete());
        EXPECT_EQ(h.hash, rig.server.Hash(client.SyncWindow()).hash) << "bot " << m->bot->Settings().id;
        EXPECT_EQ(EntityFields(net::SessionEntitiesLine(client)), EntityFields(serverEntities)) << "bot " << m->bot->Settings().id;
    }

    // The verdicts.
    const net::BotVerdict va = net::JudgeBot(*ma.bot, {});
    const net::BotVerdict vb = net::JudgeBot(*mb.bot, {});
    const net::BotVerdict vc = net::JudgeBot(*mc.bot, {});
    const net::BotVerdict ve = net::JudgeBot(*me.bot, {});
    for (const std::string& r : va.reasons) ADD_FAILURE() << "bot 0: " << r;
    for (const std::string& r : vb.reasons) ADD_FAILURE() << "bot 1: " << r;
    EXPECT_TRUE(va.ok);
    EXPECT_TRUE(vb.ok);
    EXPECT_NE(vb.checksNote.find("was found and repaired"), std::string::npos);
    EXPECT_FALSE(vc.ok);
    ASSERT_FALSE(vc.reasons.empty());
    EXPECT_NE(vc.reasons[0].find("did not put it in server terrain mode"), std::string::npos);
    EXPECT_FALSE(ve.ok);
    ASSERT_FALSE(ve.reasons.empty());
    EXPECT_NE(ve.reasons[0].find("was not found exactly"), std::string::npos);
    net::BotGates strict;
    strict.minHashChecks = 1000000;
    const net::BotVerdict vs = net::JudgeBot(*ma.bot, strict);
    EXPECT_FALSE(vs.ok);
    strict = {};
    strict.maxFinalError = -1.0f;
    EXPECT_FALSE(net::JudgeBot(*ma.bot, strict).ok);

    // The lines the scripts parse (tools/session-check.sh, tools/soak.sh).
    const net::Client& ca = ma.bot->GetClient();
    const std::string hashLine = net::SessionHashLine(ca);
    EXPECT_TRUE(std::regex_search(hashLine, std::regex(R"(Session: slot [0-9]+ applied opSeq [0-9]+ of the server's [0-9]+; world hash [0-9a-f]{16} \([0-9]+ chunks\) over \[-?[0-9]+,-?[0-9]+,-?[0-9]+\.\.-?[0-9]+,-?[0-9]+,-?[0-9]+\))")))
        << hashLine;
    // Recheck of stage 8: both lines name the client by its join point (a slot is reused after a leave:
    // tools/soak.sh matched a client to another client's leave line in the same slot).
    EXPECT_TRUE(std::regex_search(hashLine, std::regex(R"(\); joined at opSeq [0-9]+, tick [0-9]+$)"))) << hashLine;
    for (auto& m : rig.members) {
        const net::Client& c = m->bot->GetClient();
        uint32_t joinOpSeq = 0, joinTick = 0;
        ASSERT_TRUE(rig.server.ClientJoinPoint(c.Slot(), joinOpSeq, joinTick));
        EXPECT_EQ(joinOpSeq, c.JoinOpSeq());
        EXPECT_EQ(joinTick, c.JoinTick());
        const std::string point = "; joined at opSeq " + std::to_string(c.JoinOpSeq()) + ", tick " + std::to_string(c.JoinTick());
        const std::string leave = net::ServerLeaveLine(rig.server, c.Slot());
        EXPECT_TRUE(std::regex_search(leave, std::regex(R"(^Session: the client of slot [0-9]+ left; server at opSeq [0-9]+, tick [0-9]+, world hash [0-9a-f]{16} \([0-9]+ chunks\); window hash [0-9a-f]{16} over \[-?[0-9]+,-?[0-9]+,-?[0-9]+\.\.-?[0-9]+,-?[0-9]+,-?[0-9]+\); joined at opSeq [0-9]+, tick [0-9]+$)")))
            << leave;
        EXPECT_NE(leave.find(point), std::string::npos) << leave;
        EXPECT_NE(net::SessionHashLine(c).find(point), std::string::npos) << net::SessionHashLine(c);
    }
    EXPECT_NE(ma.bot->GetClient().JoinTick(), mb.bot->GetClient().JoinTick());
    uint32_t noOpSeq = 0, noTick = 0;
    EXPECT_FALSE(rig.server.ClientJoinPoint(200, noOpSeq, noTick));
    const std::string entityLine = net::SessionEntitiesLine(ca);
    EXPECT_TRUE(std::regex_search(entityLine, std::regex(R"(Session: slot [0-9]+ entities at opSeq [0-9]+: [0-9]+ object\(s\), [0-9]+ resting, id hash [0-9a-f]{16}, rest hash [0-9a-f]{16})")))
        << entityLine;
    EXPECT_TRUE(std::regex_search(serverEntities, std::regex(R"(Session: entities at opSeq [0-9]+: [0-9]+ object\(s\), [0-9]+ resting, id hash [0-9a-f]{16}, rest hash [0-9a-f]{16})")))
        << serverEntities;
    const std::string checks = net::DescribeChecks(ca);
    for (const char* field : {R"(^[0-9]+ hash checks)", R"( [0-9]+ hash mismatches)", R"( [0-9]+ entity mismatches)", R"( [0-9]+ out of order)"})
        EXPECT_TRUE(std::regex_search(checks, std::regex(field))) << field << " in " << checks;
    EXPECT_TRUE(std::regex_search(net::DescribeBotMovement(*ma.bot, 1.0 / 60.0),
                                  std::regex(R"(^Client 0 movement: .*\(([0-9.]+) per minute\).* final error [0-9.]+ m$)")));
    EXPECT_TRUE(std::regex_search(net::DescribeBotJoin(*mb.bot, 1.0 / 60.0), std::regex(R"(^Join: bot 1 started after .* KiB bulk received)")));
    const std::vector<std::string> window = net::DescribeBotWindow(*ma.bot);
    ASSERT_FALSE(window.empty());
    EXPECT_TRUE(std::regex_search(window[0], std::regex(R"(^Client 0 window: .* at most [0-9]+ used)")));
    std::printf("[ bots ] %s\n[ bots ] %s\n[ bots ] %s\n", hashLine.c_str(), entityLine.c_str(), (std::string("Checks: bot 1: ") + net::DescribeChecks(mb.bot->GetClient()) + vb.checksNote).c_str());
    const std::string awakeLine = net::SessionAwakeLine(ca);
    EXPECT_TRUE(std::regex_search(awakeLine, std::regex(R"(^Session: slot [0-9]+ awake at opSeq [0-9]+: [0-9]+ awake(: [0-9]+ \(-?[0-9.]+, -?[0-9.]+, -?[0-9.]+\)(; [0-9]+ \(-?[0-9.]+, -?[0-9.]+, -?[0-9.]+\))*)?$)")))
        << awakeLine;
    EXPECT_TRUE(std::regex_search(net::ServerAwakeLine(rig.server), std::regex(R"(^Session: awake at opSeq [0-9]+: [0-9]+ awake)")));
    std::vector<net::EntityInfo> two(2);
    two[0].netId = 9;
    two[0].position = {1.0f, -2.5f, 3.25f};
    two[1].netId = 4;
    two[1].position = {0.0f, 0.0f, 0.5f};
    EXPECT_EQ(net::AwakeEntitiesText(two), "2 awake: 4 (0.000, 0.000, 0.500); 9 (1.000, -2.500, 3.250)");
    two[1].resting = true;
    EXPECT_EQ(net::AwakeEntitiesText(two), "1 awake: 9 (1.000, -2.500, 3.250)");

    // Review of stage 8: the gates that the bot played as told. B cast about once per 90 ticks it acted;
    // asking for ten times that fails it, and so does a walk share for a bot that does not walk (judged
    // only for walkers: it passes).
    net::BotGates played;
    played.minActedShare = 0.9f;
    played.minPilotShare = 0.5f;
    played.maxRejectedCastShare = 0.2f;
    played.minWalkShare = 0.8f;
    const net::BotVerdict vp = net::JudgeBot(*mb.bot, played);
    for (const std::string& r : vp.reasons) ADD_FAILURE() << "bot 1 (played gates): " << r;
    EXPECT_GT(mb.bot->ActedTicks(), 0u);
    EXPECT_GE(mb.bot->WantedTicks(), mb.bot->ActedTicks());
    played.minPilotShare = 10.0f;
    const net::BotVerdict vp10 = net::JudgeBot(*mb.bot, played);
    EXPECT_FALSE(vp10.ok);
    ASSERT_FALSE(vp10.reasons.empty());
    EXPECT_NE(vp10.reasons.back().find("its pilot cast"), std::string::npos) << vp10.reasons.back();
    // A bot that never acted (C stands): nothing was asked of it.
    EXPECT_EQ(mc.bot->WantedTicks(), 0u);

    // Review of stage 8: B's chunk differs again after its repair (an operation applied to it differently
    // would do that): a replication bug, not the injected fault, even when the chunk is repaired again.
    sim::Simulation* bsim = mb.bot->GetSimulation();
    ASSERT_NE(bsim, nullptr);
    const glm::ivec3 v0 = broken * voxel::kChunkSize;
    voxel::MaterialLibrary scratch;
    const uint8_t sand = voxel::RegisterDefaultWorldMaterials(scratch).sand;
    ASSERT_GT(bsim->World().FillBox(v0 + glm::ivec3(3), v0 + glm::ivec3(9), sand), 0u);
    uint32_t again = 0;
    for (uint32_t t = 0; t < 1800 && again == 0; ++t) {
        rig.Step(false);
        again = mb.bot->GetClient().Sync().Stats().repeatRepairs;
    }
    EXPECT_GT(again, 0u) << "no HashCheck found the chunk again";
    const net::BotVerdict vagain = net::JudgeBot(*mb.bot, {});
    EXPECT_FALSE(vagain.ok);
    bool named = false;
    for (const std::string& r : vagain.reasons) named = named || r.find("again") != std::string::npos;
    EXPECT_TRUE(named) << (vagain.reasons.empty() ? std::string("no reason") : vagain.reasons[0]);
}

namespace {
    bool HasReason(const net::BotVerdict& v, const char* text) {
        for (const std::string& r : v.reasons)
            if (r.find(text) != std::string::npos) return true;
        return false;
    }
    std::string Reasons(const net::BotVerdict& v) {
        std::string out;
        for (const std::string& r : v.reasons) out += r + "; ";
        return out.empty() ? std::string("no reason") : out;
    }
}

// Recheck of stage 8: every gate that a bot played as it was told fails with its own reason, and passes an
// honest bot. A walker crosses the rig's play bounds (its span as the waypoints allow it: the pilot stops
// within reachMetres of each end), is judged before it reached two waypoints (the waypoint rule) and against
// more than the bounds hold (the span); a bot that casts at the sky has every cast refused by the server
// (Missed: the rejected-cast share); a client never stops predicting once it started, so an honest bot
// acts on every tick it was asked to, and only a share above 1 shows the acted gate failing.
TEST(Bot, ThePlayedGatesFailWithTheirOwnReasons) {
    BotRig rig;
    ASSERT_TRUE(rig.started);
    net::BotSettings w;
    w.id = 0;
    w.walkAcross = true;
    w.pilot.jumps = false;
    BotRig::Member& mw = rig.Add(w, 0);
    net::BotSettings k;
    k.id = 1;
    k.pilot.casts = true;
    k.pilot.castEveryTicks = 30;
    k.pilot.castPitch = 1.2f;                 // up at the sky: nothing within the cast range
    BotRig::Member& mk = rig.Add(k, 0);

    net::BotGates played;
    played.minActedShare = 0.8f;
    played.minPilotShare = 0.5f;
    played.maxRejectedCastShare = 0.2f;
    played.minWalkShare = 0.8f;
    played.maxStillSeconds = net::kWalkerMaxStillSeconds;
    played.maxDetourShare = net::kWalkerMaxDetourShare;
    bool early = false;
    for (uint32_t t = 0; t < 1500; ++t) {
        rig.Step();
        if (!early && mw.bot->GetClient().Predicting() && mw.bot->ActedTicks() >= 60) {
            early = true;
            ASSERT_LT(mw.bot->Pilot()->WaypointsReached(), 2u);
            const net::BotVerdict v = net::JudgeBot(*mw.bot, played);
            EXPECT_TRUE(HasReason(v, "it reached")) << Reasons(v);
        }
    }
    ASSERT_TRUE(early);
    std::vector<net::DrainWatch> watches(rig.members.size(), net::DrainWatch(1.0));
    bool drained = false;
    for (uint32_t t = 0; t < 1200 && !drained; ++t) {
        rig.Step(false);
        drained = true;
        for (size_t i = 0; i < rig.members.size(); ++i)
            drained = watches[i].Update(rig.members[i]->bot->GetClient(), static_cast<double>(t) / 60.0) && drained;
    }
    ASSERT_TRUE(drained);
    std::printf("[ gates ] walker: x %.1f..%.1f m, %u waypoints reached, %u given up, %llu of %llu ticks acted; sky caster: %u casts, %llu "
                "refused\n",
                static_cast<double>(mw.bot->MinX()), static_cast<double>(mw.bot->MaxX()), mw.bot->Pilot()->WaypointsReached(),
                mw.bot->Pilot()->WaypointsGivenUp(), static_cast<unsigned long long>(mw.bot->ActedTicks()),
                static_cast<unsigned long long>(mw.bot->WantedTicks()), mk.bot->Pilot()->Casts(),
                static_cast<unsigned long long>(mk.bot->GetClient().Prediction().castsRejected));

    // The walker passes every gate, and fails the span asked beyond what the bounds hold.
    const net::BotVerdict vw = net::JudgeBot(*mw.bot, played);
    EXPECT_TRUE(vw.ok) << Reasons(vw);
    EXPECT_GT(mw.bot->ActedTicks(), 0u);
    EXPECT_EQ(mw.bot->ActedTicks(), mw.bot->WantedTicks());
    // It reached every waypoint, so it covered the span the waypoints allow (the pilot stops within
    // reachMetres of each end: 16 of the waypoints' 20 m here); the gate's span must be that one, or a walker
    // that did all it was asked fails a share near 1 on a small region (it did before the recheck).
    ASSERT_GE(mw.bot->Pilot()->WaypointsReached(), 3u);
    net::BotGates whole = played;
    whole.minWalkShare = 0.95f;
    const net::BotVerdict vwhole = net::JudgeBot(*mw.bot, whole);
    EXPECT_TRUE(vwhole.ok) << Reasons(vwhole);
    net::BotGates far = played;
    far.minWalkShare = 1.5f;
    const net::BotVerdict vfar = net::JudgeBot(*mw.bot, far);
    EXPECT_FALSE(vfar.ok);
    EXPECT_TRUE(HasReason(vfar, "it walked x from")) << Reasons(vfar);
    // JudgeBot carries the walker's snag gates (WalkerSnagFailures): a bound under its longest still
    // stretch fails it.
    net::BotGates still = played;
    still.maxStillSeconds = 0.001f;
    const net::BotVerdict vstill = net::JudgeBot(*mw.bot, still);
    EXPECT_FALSE(vstill.ok);
    EXPECT_TRUE(HasReason(vstill, "it stood still")) << Reasons(vstill);
    net::BotGates acted = played;
    acted.minActedShare = 1.01f;
    const net::BotVerdict vacted = net::JudgeBot(*mw.bot, acted);
    EXPECT_FALSE(vacted.ok);
    EXPECT_TRUE(HasReason(vacted, "its pilot drove")) << Reasons(vacted);

    // The sky caster cast as often as asked, and the server ran none of it.
    EXPECT_GT(mk.bot->Pilot()->Casts(), 10u);
    EXPECT_EQ(mk.bot->GetClient().Prediction().castsRejected, mk.bot->Pilot()->Casts());
    const net::BotVerdict vk = net::JudgeBot(*mk.bot, played);
    EXPECT_FALSE(vk.ok);
    EXPECT_TRUE(HasReason(vk, "the server did not run")) << Reasons(vk);
    EXPECT_EQ(vk.reasons.size(), 1u) << Reasons(vk);
    net::BotGates lax = played;
    lax.maxRejectedCastShare = 1.0f;
    EXPECT_TRUE(net::JudgeBot(*mk.bot, lax).ok) << Reasons(net::JudgeBot(*mk.bot, lax));
}

namespace {
    /// A loopback endpoint whose Flush costs `work` seconds of a manual clock: the time a bot's Pass spends
    /// on its ticks, loads and installs (Bot::Pass flushes its link once at its end).
    class SlowLink final : public net::ITransport {
    public:
        SlowLink(double& clock, double work) : m_Clock(clock), m_Work(work) {}
        net::LoopbackTransport loop;
        void Send(net::PeerId peer, net::Channel channel, std::span<const uint8_t> data) override { loop.Send(peer, channel, data); }
        bool Poll(net::TransportEvent& out) override { return loop.Poll(out); }
        double RoundTripSeconds(net::PeerId peer) const override { return loop.RoundTripSeconds(peer); }
        size_t ReliableBytesInFlight(net::PeerId peer) const override { return loop.ReliableBytesInFlight(peer); }
        size_t ChannelBytesInFlight(net::PeerId peer, net::Channel channel) const override { return loop.ChannelBytesInFlight(peer, channel); }
        void Disconnect(net::PeerId peer) override { loop.Disconnect(peer); }
        void Flush() override { m_Clock += m_Work; }

    private:
        double& m_Clock;
        double m_Work;
    };
}

// Review of stage 8: vortex_bot's wall-clock pass left the time spent inside each Pass out of its clock,
// so the bot's clock ran slow and kept jumping forward (1-4 jumps a second in the 10-minute soak). With
// passes that spend 6 ms inside and sleep 1 ms between, Bot::PassOnClock must give the client the time
// that really passed: as many ticks as the server's, and no clock jump once it plays.
TEST(Bot, PassOnClockCountsTheWorkInsidePasses) {
    double now = 0.0;
    net::LoopbackTransport serverLoop;
    net::Server server(RigSettings(), serverLoop);
    ASSERT_TRUE(server.Start());
    SlowLink link(now, 0.006);
    net::BotSettings s;
    s.client.token = RigToken();
    s.chunkCapacity = 4096;
    s.pilot.jumps = false;
    net::Bot bot(s, link);
    net::LoopbackTransport::Connect(serverLoop, link.loop);
    const auto clock = [&now] { return now; };
    uint64_t ticksAt10 = 0, jumpsAt10 = 0, resyncsAt10 = 0;
    bool marked = false;
    while (now < 30.0) {
        now += 0.001;                                                    // the sleep between passes
        while (static_cast<double>(server.TickNumber()) < now * 60.0) {  // the server on the same clock
            server.Tick();
            server.Simulation().Jobs().WaitIdle();
        }
        bot.PassOnClock(clock, true);
        if (sim::Simulation* sim = bot.GetSimulation()) sim->Jobs().WaitIdle();
        if (!marked && now >= 10.0) {
            marked = true;
            const net::PredictionStats& p = bot.GetClient().Prediction();
            ticksAt10 = p.ticks;
            jumpsAt10 = p.clockJumps;
            resyncsAt10 = p.hardResyncs;
        }
    }
    const net::PredictionStats& p = bot.GetClient().Prediction();
    std::printf("[ pass clock ] %llu ticks predicted in the last 20 s (1200 due), %llu clock jumps, %llu resyncs (in all %llu / %llu)\n",
                static_cast<unsigned long long>(p.ticks - ticksAt10), static_cast<unsigned long long>(p.clockJumps - jumpsAt10),
                static_cast<unsigned long long>(p.hardResyncs - resyncsAt10), static_cast<unsigned long long>(p.clockJumps),
                static_cast<unsigned long long>(p.hardResyncs));
    ASSERT_TRUE(bot.GetClient().Predicting());
    EXPECT_NEAR(static_cast<double>(p.ticks - ticksAt10), 1200.0, 24.0);
    EXPECT_EQ(p.clockJumps - jumpsAt10, 0u);
    EXPECT_EQ(p.hardResyncs - resyncsAt10, 0u);
}

// The server's timeline gives each event the time it was made; the bot's applied times give the latency.
TEST(Bot, LatencyFromTheServersTimeline) {
    const std::string text =
        "# vortex_server timeline\n"
        "ops 3 1000000000 0\n"     // opSeq 1..3 made at 1.000 s
        "ops 5 1100000000 1\n"     // 4..5 at 1.100 s, a client joining
        "ops 5 1200000000 0\n"     // the join ended
        "ops 9 1300000000 0\n"     // 6..9 at 1.300 s
        "ops 12 14000";            // a last line still being written: ignored
    std::vector<net::TimelineEntry> timeline;
    std::string error;
    ASSERT_TRUE(net::ParseTimeline(text, timeline, &error)) << error;
    ASSERT_EQ(timeline.size(), 4u);
    EXPECT_EQ(timeline[1].joining, 1u);
    // Applied: 1..3 at 1.050 s (before the bot played at 1.05: counted apart), 4..5 at 1.150, 6..9 at 1.400 and 1.500.
    std::vector<std::pair<uint32_t, double>> applied;
    for (uint32_t k = 1; k <= 3; ++k) applied.emplace_back(k, 1.050);
    for (uint32_t k = 4; k <= 5; ++k) applied.emplace_back(k, 1.150);
    for (uint32_t k = 6; k <= 7; ++k) applied.emplace_back(k, 1.400);
    for (uint32_t k = 8; k <= 9; ++k) applied.emplace_back(k, 1.500);
    applied.emplace_back(10, 1.6);    // beyond the timeline
    const net::EventLatency l = net::LatencyFromTimeline(timeline, applied, 1.05);
    EXPECT_EQ(l.beforePlaying, 3u);
    EXPECT_NEAR(l.beforePlayingMaxMs, 50.0, 1e-6);
    EXPECT_EQ(l.unknown, 1u);
    EXPECT_EQ(l.all.count, 6u);
    EXPECT_NEAR(l.all.max, 200.0, 1e-6);
    EXPECT_NEAR(l.all.p50, 100.0, 1e-6);    // {50, 50, 100, 100, 200, 200}: rank 3
    EXPECT_EQ(l.duringJoins.count, 2u);
    EXPECT_NEAR(l.duringJoins.max, 50.0, 1e-6);
    EXPECT_EQ(l.quiet.count, 4u);             // review of stage 8: the events made while nobody joined
    EXPECT_NEAR(l.quiet.p50, 100.0, 1e-6);    // {100, 100, 200, 200}
    EXPECT_NEAR(l.quiet.max, 200.0, 1e-6);

    EXPECT_FALSE(net::ParseTimeline("ops 1 10 0\nops x 20 0\n", timeline, &error));
    EXPECT_NE(error.find("line 2"), std::string::npos) << error;
    EXPECT_FALSE(net::ParseTimeline("ops 5 10 0\nops 4 20 0\n", timeline, &error));
    EXPECT_NE(error.find("goes back"), std::string::npos) << error;

    const net::Percentiles p = net::PercentilesOf({5, 1, 4, 2, 3});
    EXPECT_EQ(p.count, 5u);
    EXPECT_DOUBLE_EQ(p.p50, 3.0);
    EXPECT_DOUBLE_EQ(p.p99, 5.0);
    EXPECT_DOUBLE_EQ(p.max, 5.0);
}

// Recheck of stage 8: vortex_server's pass times go into a fixed histogram (it kept every one of a
// listening server's passes, about 0.9 MB an hour, and sorted a copy at exit). Its percentiles stay within
// one bucket of the exact ones, the overflow bucket answers with the largest value, and its memory does
// not grow with the values.
TEST(Bot, DurationHistogramKeepsPercentilesInFixedMemory) {
    net::DurationHistogram h;                 // 0.05 ms buckets up to 100 ms
    EXPECT_EQ(h.Summary().count, 0u);
    EXPECT_EQ(h.Percentile(0.99), 0.0);
    std::vector<double> values;
    for (uint32_t i = 0; i < 200000; ++i) {
        const double v = 0.0002 + static_cast<double>((i * 7919u) % 10007u) * 1e-6 + (i % 97 == 0 ? 0.012 : 0.0);   // 0.2-10.2 ms, 1 % +12 ms
        values.push_back(v);
        h.Add(v);
    }
    const net::Percentiles exact = net::PercentilesOf(values);
    const net::Percentiles fixed = h.Summary();
    EXPECT_EQ(fixed.count, exact.count);
    EXPECT_DOUBLE_EQ(fixed.max, exact.max);
    EXPECT_GE(fixed.p50, exact.p50);
    EXPECT_LE(fixed.p50, exact.p50 + 0.00005);
    EXPECT_GE(fixed.p99, exact.p99);
    EXPECT_LE(fixed.p99, exact.p99 + 0.00005);
    // Over 1 % beyond the last bucket: the p99 is the largest value.
    for (int i = 0; i < 3000; ++i) h.Add(0.25 + 0.001 * i);
    EXPECT_DOUBLE_EQ(h.Percentile(0.99), h.Max());
    EXPECT_DOUBLE_EQ(h.Max(), 0.25 + 0.001 * 2999);
    EXPECT_LT(h.Percentile(0.5), 0.011);
}

// G19 failed on 2026-11-22 (arena, bot 0: "it reached 17 waypoints and gave up 43"). About 30 of those
// give-ups came from one 150 s stretch in which the walker stood still for a reason not yet known (the
// walk line now names the longest still stretch and what was near it). Trees on the walker's line (a
// one-voxel trunk under a canopy that hangs 1.6 m above the ground: the capsule's head meets it, a jump
// only lifts it into the leaves) are a contributing weakness: a straight pusher was held behind them for
// 5 s now and then. On flat ground with such a tree and a wall it cannot jump (3 m high, 4 m wide) across
// its line, the soak's walker (run, jumps, --loop) must go round both, both ways, every lap: no waypoint
// given up.
TEST(Pilot, AWalkerGoesRoundATreeAndAWallOnItsLine) {
    sim::RegionDesc region;
    region.templateId = sim::RegionTemplateId::Empty;
    region.seed = 31;
    region.playMinChunk = {-8, -3};
    region.playMaxChunk = {8, 4};
    region.heightRange = {0.0f, 32.0f};
    region.generator = sim::RegionGenerator::Flat;
    region.flatGroundVoxels = 100;                       // ground top at 12.5 m
    region.terrain.voxelSize = 0.125f;
    voxel::MaterialLibrary scratch;
    const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
    region.terrain.materials = ids;
    ASSERT_TRUE(sim::IsValid(region));
    sim::SimulationConfig config;
    config.maxParticles = 0;
    config.jobThreads = 2;
    config.physics.threads = 1;
    config.chunkCapacity = sim::RegionChunkCapacity(region);
    config.world = sim::RegionWorldConfig(region, config.world);
    sim::Simulation simulation(config);
    ASSERT_TRUE(simulation.Initialize(sim::MakeRegionGenerator(region, ids)));
    simulation.World().LoadBlocking(sim::RegionWindowFocus(region));
    voxel::World& world = simulation.World();
    // Voxels: the line is z = 16 (2 m). The tree at x = -5 m: its trunk beside the line (z 2.5 m), its
    // canopy over it from 1.6 to 3 m above the ground (the arena's at x -92.7 m). The wall at x = +5 m.
    world.FillBox({-40, 100, 20}, {-39, 140, 21}, ids.wood);
    world.FillBox({-52, 113, 4}, {-28, 124, 36}, ids.leaves);
    world.FillBox({40, 100, 0}, {44, 124, 32}, ids.stone);
    const sim::PlayerTuning tuning;
    const sim::PlayArea area = sim::PlayAreaOf(region, tuning);
    sim::SetPlayAreaWalls(simulation.Physics(), area, tuning);
    sim::PlayerState state;
    state.position = glm::vec3(-20.0f, 12.5f, 2.0f);
    const physics::CharacterId character = simulation.Physics().CreateCharacter(sim::PlayerCharacterDesc(state.position, tuning));
    net::PilotSettings ps;                               // as vortex_bot --walk-across --loop (BuildAndAttach)
    ps.waypoints = {{-24.0f, 2.0f}, {24.0f, 2.0f}, {0.0f, 2.0f}};
    ps.loopWaypoints = true;
    ps.casts = false;
    net::BotPilot pilot(region, 0xb07u, ps);
    float minX = state.position.x, maxX = state.position.x;
    uint32_t given = 0;
    for (int t = 0; t < 60 * 60; ++t) {
        const std::array<glm::dvec3, 1> focus = {glm::dvec3(state.position)};
        simulation.UpdateStreaming(focus);
        simulation.Terrain().FlushBox(sim::PlayerReach(state.position, tuning));
        const sim::PlayerInput in = pilot.Next(state);
        if (pilot.WaypointsGivenUp() != given) {
            given = pilot.WaypointsGivenUp();
            ADD_FAILURE() << "tick " << t << ": a waypoint given up at (" << state.position.x << ", " << state.position.y << ", "
                          << state.position.z << ")";
        }
        sim::StepPlayer(simulation.Physics(), simulation.Terrain(), character, state, in, tuning, area, simulation.FixedTimeStep());
        simulation.StepPhysics();
        if (simulation.Terrain().Stats().pending != 0) simulation.Jobs().WaitIdle();
        minX = std::min(minX, state.position.x);
        maxX = std::max(maxX, state.position.x);
    }
    std::printf("[ pilot ] %u waypoints reached, %u given up, %u laps; %u detours (%u ticks); x from %.1f to %.1f m\n",
                pilot.WaypointsReached(), pilot.WaypointsGivenUp(), pilot.Laps(), pilot.Detours(), pilot.DetourTicks(),
                static_cast<double>(minX), static_cast<double>(maxX));
    EXPECT_EQ(pilot.WaypointsGivenUp(), 0u);
    EXPECT_TRUE(pilot.GiveUps().empty());
    EXPECT_GE(pilot.Detours(), 4u);                      // the tree and the wall held it each way
    EXPECT_GE(pilot.WaypointsReached(), 9u);             // 3 laps of 120 m at 7 m/s, the detours included
    // It goes round, so the snag gates hold: never still for long, a small share of its ticks detouring.
    net::BotGates gates;
    gates.maxStillSeconds = net::kWalkerMaxStillSeconds;
    gates.maxDetourShare = net::kWalkerMaxDetourShare;
    gates.maxConfinedSeconds = net::kWalkerMaxConfinedSeconds;
    const std::vector<std::string> snags = net::WalkerSnagFailures(pilot, gates, simulation.FixedTimeStep());
    EXPECT_TRUE(snags.empty()) << (snags.empty() ? std::string() : snags[0]);
    // Going round the tree and the wall keeps it within 4 m for a few seconds at most, nowhere near 45 s.
    EXPECT_LE(pilot.LongestConfined().ticks, 60u * 10u);
    EXPECT_EQ(pilot.LongestStill().heldTicks, 0u);       // its terrain had colliders throughout
    std::printf("[ pilot ] %u of %u walking ticks detouring, longest still %u ticks, longest within 4 m %u ticks\n", pilot.DetourTicks(),
                pilot.WalkTicks(), pilot.LongestStill().ticks, pilot.LongestConfined().ticks);
}

namespace {
    // A walker on a plane without physics (BotPilot only, GPU-free): each tick its feet move 7 m/s / 60 the
    // way its input points, except into `blocked` (a wall: the move along x is refused, z slides on).
    struct PlaneWalk {
        sim::RegionDesc region;
        net::BotPilot pilot;
        sim::PlayerState state;
        std::function<bool(glm::vec2)> blocked;
        explicit PlaneWalk(net::PilotSettings ps) : region(PlaneRegion()), pilot(region, 0x5aau, std::move(ps)) {}
        static sim::RegionDesc PlaneRegion() {
            sim::RegionDesc r;
            r.playMinChunk = {-40, -40};
            r.playMaxChunk = {40, 40};
            r.terrain.voxelSize = 0.125f;
            return r;
        }
        void Run(uint32_t ticks, bool move = true) {
            for (uint32_t t = 0; t < ticks; ++t) {
                // Not moving: as StepPlayer's collider gate holds a player (PlayerFlag_Held).
                state.flags = move ? 0 : sim::PlayerFlag_Held;
                const sim::PlayerInput in = pilot.Next(state);
                if (!move || in.moveZ == 0) continue;
                const float yaw = sim::YawRadians(in.yaw);
                const glm::vec2 step = glm::vec2(-std::sin(yaw), -std::cos(yaw)) * (7.0f / 60.0f);
                glm::vec2 to(state.position.x + step.x, state.position.z + step.y);
                if (blocked && blocked(to)) to.x = state.position.x;
                if (blocked && blocked(to)) to = glm::vec2(state.position.x, state.position.z);
                state.position = glm::vec3(to.x, 0.0f, to.y);
            }
        }
    };
}

// A walker that goes round whatever holds it must not hide one that snags (the review of the 2026-11-22
// G19 fix): the verdict bounds its longest still stretch, its share of ticks detouring and its longest
// confined stretch (net::WalkerSnagFailures, in JudgeBot for vortex_bot and vortex_server). One hold is
// one detour, however many legs aside it takes, and every tick of it counts; ticks held by the collider
// gate are counted in each stretch.
TEST(Pilot, TheSnagGatesFailAStillOrDetouringWalkerAndPassAFreeOne) {
    net::PilotSettings ps;
    ps.waypoints = {{100.0f, 0.0f}, {-100.0f, 0.0f}};
    ps.loopWaypoints = true;
    ps.jumps = false;
    net::BotGates gates;
    gates.maxStillSeconds = net::kWalkerMaxStillSeconds;
    gates.maxDetourShare = net::kWalkerMaxDetourShare;
    gates.maxConfinedSeconds = net::kWalkerMaxConfinedSeconds;
    const double tick = 1.0 / 60.0;

    // Free: it runs to and fro, never still, never detouring.
    PlaneWalk free(ps);
    free.Run(60 * 120);
    EXPECT_GE(free.pilot.WaypointsReached(), 3u);
    EXPECT_EQ(free.pilot.WaypointsGivenUp(), 0u);
    EXPECT_EQ(free.pilot.Detours(), 0u);
    EXPECT_EQ(free.pilot.DetourTicks(), 0u);
    EXPECT_EQ(free.pilot.WalkTicks(), 60u * 120u);
    EXPECT_LE(free.pilot.LongestStill().ticks, 20u);     // a turn at a waypoint: out and back within 1 m
    EXPECT_LE(free.pilot.LongestConfined().ticks, 75u);  // and within 4 m: 8 m out and back at 7 m/s
    EXPECT_EQ(free.pilot.LongestConfined().heldTicks, 0u);
    EXPECT_TRUE(net::WalkerSnagFailures(free.pilot, gates, tick).empty());

    // Held in place (its feet never move): 25 s still at the start, every waypoint given up after 5 s and
    // all but the first 45 ticks of each spent detouring (85 %). Both gates fail, each with its own phrase.
    PlaneWalk held(ps);
    held.state.position = glm::vec3(3.0f, 1.0f, 2.0f);
    held.Run(60 * 25, false);
    EXPECT_EQ(held.pilot.LongestStill().ticks, 60u * 25u);
    EXPECT_EQ(held.pilot.LongestStill().startTick, 1u);
    EXPECT_EQ(held.pilot.LongestStill().at, glm::vec3(3.0f, 1.0f, 2.0f));
    EXPECT_EQ(held.pilot.LongestStill().heldTicks, 60u * 25u);          // every tick held (Run: not moving)
    EXPECT_EQ(held.pilot.LongestConfined().heldTicks, 60u * 25u);
    EXPECT_EQ(held.pilot.WaypointsGivenUp(), 4u);
    EXPECT_EQ(held.pilot.Detours(), 5u);                 // one per waypoint held, the fifth going on
    const std::vector<std::string> both = net::WalkerSnagFailures(held.pilot, gates, tick);
    ASSERT_EQ(both.size(), 2u);                          // 25 s confined is under 45 s
    EXPECT_NE(both[0].find("it stood still 25.0 s at (3.0, 1.0, 2.0) from pilot tick 1"), std::string::npos) << both[0];
    EXPECT_NE(both[1].find("detouring"), std::string::npos) << both[1];
    // Each gate alone, and 0 = not judged.
    net::BotGates stillOnly = gates;
    stillOnly.maxDetourShare = 0.0f;
    EXPECT_EQ(net::WalkerSnagFailures(held.pilot, stillOnly, tick).size(), 1u);
    net::BotGates lax = gates;
    lax.maxStillSeconds = 30.0f;
    lax.maxDetourShare = 0.9f;
    EXPECT_TRUE(net::WalkerSnagFailures(held.pilot, lax, tick).empty());

    // Still under the bound (15 s), then free: no still failure; and the stretch ends with a metre moved.
    PlaneWalk paused(ps);
    paused.Run(60 * 15, false);
    paused.Run(60 * 120);
    EXPECT_GE(paused.pilot.LongestStill().ticks, 60u * 15u);
    EXPECT_LE(paused.pilot.LongestStill().ticks, 60u * 15u + 12u);  // and the first steps that stay within 1 m
    net::BotGates stillGate = gates;
    stillGate.maxDetourShare = 0.0f;
    EXPECT_TRUE(net::WalkerSnagFailures(paused.pilot, stillGate, tick).empty());

    // A wall across its line (x 10 to 11 m, z under 1.5 m): it goes round by one detour, whichever side it
    // tries first, and loses only a small share of its ticks to it.
    net::PilotSettings once = ps;
    once.loopWaypoints = false;
    once.waypoints = {{100.0f, 0.0f}};
    PlaneWalk wall(once);
    wall.blocked = [](glm::vec2 p) { return p.x > 10.0f && p.x < 11.0f && p.y < 1.5f; };
    wall.Run(60 * 30);
    EXPECT_EQ(wall.pilot.WaypointsReached(), 1u);
    EXPECT_EQ(wall.pilot.WaypointsGivenUp(), 0u);
    EXPECT_EQ(wall.pilot.Detours(), 1u);
    EXPECT_GE(wall.pilot.DetourTicks(), 30u);
    EXPECT_LE(wall.pilot.LongestStill().ticks, 60u);
    EXPECT_LE(wall.pilot.LongestConfined().ticks, 60u * 5u);
    EXPECT_TRUE(net::WalkerSnagFailures(wall.pilot, gates, tick).empty());
    std::printf("[ snag ] wall: %u detour, %u of %u walking ticks detouring, longest still %u ticks, within 4 m %u ticks\n",
                wall.pilot.Detours(), wall.pilot.DetourTicks(), wall.pilot.WalkTicks(), wall.pilot.LongestStill().ticks,
                wall.pilot.LongestConfined().ticks);

    // In a pit (it cannot leave 3 m round where it stands) for 75 s, then free for 10 minutes: each step
    // aside of its detours carries it over 1 m, so the still stretch keeps starting again (under 20 s),
    // and 75 s of detours are under 15 % of the run; only the confined stretch (within 4 m) fails it.
    PlaneWalk pit(ps);
    pit.blocked = [](glm::vec2 p) { return glm::length(p) > 3.0f; };
    pit.Run(60 * 75);
    EXPECT_GE(pit.pilot.WaypointsGivenUp(), 12u);        // about one every 5 s (13 measured: detours restart the clock)
    pit.blocked = nullptr;
    pit.Run(60 * 600);
    EXPECT_LE(pit.pilot.LongestStill().ticks, 60u * 20u);
    EXPECT_GE(pit.pilot.LongestConfined().ticks, 60u * 75u);
    EXPECT_LE(pit.pilot.LongestConfined().ticks, 60u * 75u + 75u);   // and the first steps out
    EXPECT_EQ(pit.pilot.LongestConfined().startTick, 1u);
    EXPECT_EQ(pit.pilot.LongestConfined().heldTicks, 0u);
    const std::vector<std::string> shuffled = net::WalkerSnagFailures(pit.pilot, gates, tick);
    ASSERT_EQ(shuffled.size(), 1u) << shuffled[0];
    EXPECT_NE(shuffled[0].find("it stayed within 4 m 75."), std::string::npos) << shuffled[0];
    EXPECT_NE(shuffled[0].find("from pilot tick 1 (over 45 s; 0.0 s of it held)"), std::string::npos) << shuffled[0];
    net::BotGates noConfined = gates;
    noConfined.maxConfinedSeconds = 0.0f;
    EXPECT_TRUE(net::WalkerSnagFailures(pit.pilot, noConfined, tick).empty());
    std::printf("[ snag ] pit: %u given up, %u of %u walking ticks detouring (%.1f %%), longest still %u ticks, within 4 m %u ticks\n",
                pit.pilot.WaypointsGivenUp(), pit.pilot.DetourTicks(), pit.pilot.WalkTicks(),
                100.0 * pit.pilot.DetourTicks() / pit.pilot.WalkTicks(), pit.pilot.LongestStill().ticks, pit.pilot.LongestConfined().ticks);
}

// The relay shapes real UDP below ENet (wall clock; registered RUN_SERIAL): a 256 kB/s downlink with 50 ms
// each way delays 200 KB of reliable bulk for at least its length, ENet's round trip sees the delay, the
// server's bytes in flight grow while the relay queues, everything arrives intact, and per-channel bytes
// add up. Seeded loss drops both ways, an outage passes nothing (judged by the times the relay read each datagram:
// RelaySettings::onAdmit, recheck of stage 8), and a stranger's datagrams are dropped.
TEST(Relay, ShapesRealUdpTraffic) {
    std::string why;
    net::EnetSettings es;
    std::unique_ptr<net::EnetTransport> server = net::EnetTransport::Listen("127.0.0.1", 0, es, &why);
    ASSERT_TRUE(server) << why;
    net::RelaySettings rs;
    rs.serverHost = "127.0.0.1";
    rs.serverPort = server->Port();
    rs.up.delaySeconds = rs.down.delaySeconds = 0.05;
    rs.down.bytesPerSecond = 256.0 * 1024.0;
    std::unique_ptr<net::UdpRelay> relay = net::UdpRelay::Start(rs, &why);
    ASSERT_TRUE(relay) << why;
    std::unique_ptr<net::EnetTransport> client = net::EnetTransport::Connect("127.0.0.1", relay->Port(), es, &why);
    ASSERT_TRUE(client) << why;

    net::PeerId serverPeer = 0;
    bool clientUp = false;
    std::vector<uint8_t> got;
    size_t messages = 0;
    auto pump = [&] {
        net::TransportEvent e;
        while (server->Poll(e))
            if (e.type == net::TransportEvent::Type::Connected) serverPeer = e.peer;
        while (client->Poll(e)) {
            if (e.type == net::TransportEvent::Type::Connected) clientUp = true;
            if (e.type == net::TransportEvent::Type::Received && e.channel == net::Channel::Bulk) {
                got.insert(got.end(), e.data.begin(), e.data.end());
                ++messages;
            }
        }
        server->Flush();
        client->Flush();
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((serverPeer == 0 || !clientUp) && std::chrono::steady_clock::now() < deadline) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_NE(serverPeer, 0u);
    ASSERT_TRUE(clientUp);

    // A stranger's datagrams (its connect attempts) reach the relay's client port: dropped.
    std::unique_ptr<net::EnetTransport> stranger = net::EnetTransport::Connect("127.0.0.1", relay->Port(), es, &why);
    ASSERT_TRUE(stranger);
    stranger->SetCloseWait(0);

    // 200 KiB in 10 KiB messages, handed over while under 32 KiB are in flight (as the server's bulk pacer
    // at its ceiling: a burst of the whole lot would overflow the relay's drop-tail queue, as a router's).
    std::vector<uint8_t> sent;
    uint32_t next = 0;
    const auto t0 = std::chrono::steady_clock::now();
    size_t maxInFlight = 0;
    while ((next < 20 || got.size() < sent.size()) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) {
        while (next < 20 && server->ChannelBytesInFlight(serverPeer, net::Channel::Bulk) < 32u * 1024u) {
            std::vector<uint8_t> msg(10 * 1024);
            for (size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<uint8_t>(next * 31 + i * 7);
            server->Send(serverPeer, net::Channel::Bulk, msg);
            sent.insert(sent.end(), msg.begin(), msg.end());
            ++next;
        }
        pump();
        net::TransportEvent ignored;
        stranger->Poll(ignored);
        stranger->Flush();
        maxInFlight = std::max(maxInFlight, server->ChannelBytesInFlight(serverPeer, net::Channel::Bulk));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[ relay ] 200 KiB through 256 KiB/s + 50 ms: %.2f s, %zu messages, RTT %.1f ms, at most %zu B in flight, down queue %.0f ms at most\n",
                seconds, messages, client->RoundTripSeconds(1) * 1000.0, maxInFlight, relay->Down().maxQueueSeconds * 1000.0);
    EXPECT_EQ(next, 20u);
    EXPECT_EQ(got, sent);
    EXPECT_GE(seconds, 0.75);                 // the bottleneck (0.78 s for the payload alone)
    EXPECT_GE(client->RoundTripSeconds(1), 0.1);
    EXPECT_GE(maxInFlight, 32u * 1024u);      // the bytes in flight include the relay's queue
    EXPECT_GT(relay->Up().strangers, 0u);
    const net::EnetStats& cs = client->Stats();
    EXPECT_EQ(cs.channelBytesReceived[0] + cs.channelBytesReceived[1] + cs.channelBytesReceived[2], cs.bytesReceived);
    const net::EnetStats& ss = server->Stats();
    EXPECT_EQ(ss.channelBytesSent[0] + ss.channelBytesSent[1] + ss.channelBytesSent[2], ss.bytesSent);

    // Loss both ways and an outage: a second relay, state datagrams every 2 ms each way for 1.5 s.
    stranger.reset();
    client.reset();
    relay.reset();
    // Recheck of stage 8: the outage is judged by the relay's own clock. Each ping carries a marker and its
    // number (State: unreliable, never re-sent; ENet puts its bytes in the datagram as they are), and the
    // relay's admit hook records when it read each one and whether it dropped it there. A thread that
    // stamps a ping, is preempted and flushes it after the outage (or a relay thread that reads it late)
    // then cannot make an arrival look like a datagram the outage let through.
    static constexpr char kMarker[8] = {'V', 'X', 'R', 'P', 'I', 'N', 'G', '!'};
    struct Admitted {
        double at = -1.0;
        bool dropped = false;
    };
    std::mutex admitMutex;
    std::vector<Admitted> admitted[2];        // [direction][ping number]
    net::RelaySettings lossy;
    lossy.serverHost = "127.0.0.1";
    lossy.serverPort = server->Port();
    lossy.up.loss = lossy.down.loss = 0.05;
    lossy.onAdmit = [&](int direction, std::span<const uint8_t> d, double at, bool dropped) {
        for (size_t i = 0; i + sizeof kMarker + 4 <= d.size(); ++i) {
            if (std::memcmp(d.data() + i, kMarker, sizeof kMarker) != 0) continue;
            uint32_t n = 0;
            std::memcpy(&n, d.data() + i + sizeof kMarker, 4);
            if (n >= 100000) continue;
            std::lock_guard lock(admitMutex);
            std::vector<Admitted>& v = admitted[direction];
            if (v.size() <= n) v.resize(n + 1);
            v[n] = {at, dropped};
        }
    };
    relay = net::UdpRelay::Start(lossy, &why);
    ASSERT_TRUE(relay) << why;
    client = net::EnetTransport::Connect("127.0.0.1", relay->Port(), es, &why);
    ASSERT_TRUE(client);
    serverPeer = 0;
    clientUp = false;
    // ENet's handshake through 5 % loss takes one resend (0.5 s and more) now and then: the outage starts
    // from the connection, not from the relay's start (review of stage 8: a fixed window could be missed).
    while ((serverPeer == 0 || !clientUp) && relay->Seconds() < 15.0) {
        net::TransportEvent e;
        while (server->Poll(e))
            if (e.type == net::TransportEvent::Type::Connected) serverPeer = e.peer;
        while (client->Poll(e))
            if (e.type == net::TransportEvent::Type::Connected) clientUp = true;
        server->Flush();
        client->Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(clientUp && serverPeer != 0);
    const double c0 = relay->Seconds();
    const double outageFrom = c0 + 0.6, outageTo = c0 + 0.9;
    relay->AddOutage(outageFrom, outageTo);
    uint32_t sentPings[2] = {0, 0};
    std::vector<uint32_t> arrived[2];         // ping numbers received: [0] at the server, [1] at the client
    const auto ping = [](uint32_t n) {
        std::vector<uint8_t> p(32, 7);
        std::memcpy(p.data(), kMarker, sizeof kMarker);
        std::memcpy(p.data() + sizeof kMarker, &n, 4);
        return p;
    };
    const auto number = [](const std::vector<uint8_t>& data, uint32_t& n) {
        if (data.size() != 32 || std::memcmp(data.data(), kMarker, sizeof kMarker) != 0) return false;
        std::memcpy(&n, data.data() + sizeof kMarker, 4);
        return true;
    };
    while (relay->Seconds() < c0 + 1.5) {
        client->Send(1, net::Channel::State, ping(sentPings[0]++));
        client->Flush();
        server->Send(serverPeer, net::Channel::State, ping(sentPings[1]++));
        net::TransportEvent e;
        uint32_t n = 0;
        while (server->Poll(e))
            if (e.type == net::TransportEvent::Type::Received && number(e.data, n)) arrived[0].push_back(n);
        while (client->Poll(e))
            if (e.type == net::TransportEvent::Type::Received && number(e.data, n)) arrived[1].push_back(n);
        server->Flush();
        client->Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Judged per direction on the relay's times: nothing it read inside the outage arrives, nothing
    // arrives that it never read, it read pings inside the outage (and dropped them all), and most of
    // those it forwarded arrived.
    std::vector<Admitted> seen[2];
    {
        std::lock_guard lock(admitMutex);   // a copy: the relay's thread calls the hook under its stats lock
        seen[0] = admitted[0];
        seen[1] = admitted[1];
    }
    uint32_t inOutage = 0, unseen = 0, readInOutage[2] = {0, 0}, forwardedInOutage = 0;
    for (int d = 0; d < 2; ++d) {
        uint32_t forwarded = 0;
        for (const Admitted& a : seen[d]) {
            if (a.at < 0.0) continue;
            const bool in = a.at >= outageFrom && a.at < outageTo;
            readInOutage[d] += in ? 1u : 0u;
            forwardedInOutage += in && !a.dropped ? 1u : 0u;
            forwarded += a.dropped ? 0u : 1u;
        }
        for (const uint32_t n : arrived[d]) {
            if (n >= seen[d].size() || seen[d][n].at < 0.0) {
                ++unseen;
                continue;
            }
            inOutage += seen[d][n].at >= outageFrom && seen[d][n].at < outageTo ? 1u : 0u;
        }
        EXPECT_GE(arrived[d].size(), forwarded / 2) << "direction " << d << ": " << forwarded << " pings forwarded";
        EXPECT_GT(arrived[d].size(), 10u) << "direction " << d;
        EXPECT_GT(readInOutage[d], 0u) << "direction " << d;
    }
    std::printf("[ relay ] loss: up %llu / %llu lost, down %llu / %llu lost; outage dropped %llu up, %llu down; pings read in it %u up, %u "
                "down; %zu / %zu received, %u of them read in the outage\n",
                static_cast<unsigned long long>(relay->Up().lost), static_cast<unsigned long long>(relay->Up().datagramsIn),
                static_cast<unsigned long long>(relay->Down().lost), static_cast<unsigned long long>(relay->Down().datagramsIn),
                static_cast<unsigned long long>(relay->Up().outage), static_cast<unsigned long long>(relay->Down().outage), readInOutage[0],
                readInOutage[1], arrived[0].size(), arrived[1].size(), inOutage);
    EXPECT_EQ(inOutage, 0u);
    EXPECT_EQ(forwardedInOutage, 0u);
    EXPECT_EQ(unseen, 0u);
    EXPECT_GT(relay->Up().outage, 0u);
    EXPECT_GT(relay->Down().outage, 0u);
    const double upLoss = static_cast<double>(relay->Up().lost) / static_cast<double>(relay->Up().datagramsIn);
    const double downLoss = static_cast<double>(relay->Down().lost) / static_cast<double>(relay->Down().datagramsIn);
    EXPECT_GT(upLoss, 0.02);
    EXPECT_LT(upLoss, 0.09);
    EXPECT_GT(downLoss, 0.02);
    EXPECT_LT(downLoss, 0.09);
}

// [wp-7] fight.md "Bots": a fighting pilot holds the nearest enemy, aims its eye at the enemy's feet, waits
// reactionTicks before its first cast and casts on edges only (one tick down, then up); not while it may not cast.
TEST(Bot, FightPilotAimsAtTheNearestEnemyAndCastsOnEdges) {
    sim::RegionDesc region;
    region.playMinChunk = {-8, -8};
    region.playMaxChunk = {8, 8};
    region.terrain.voxelSize = 0.125f;
    net::PilotSettings ps;
    ps.fight = true;
    ps.jumps = false;
    net::BotPilot pilot(region, 5, ps);
    sim::PlayerState state;
    state.position = glm::vec3(0.0f, 10.0f, 0.0f);
    const std::array<net::PilotTarget, 2> targets = {{{3, glm::vec3(0.0f, 10.0f, -20.0f)}, {2, glm::vec3(10.0f, 10.0f, 0.0f)}}};
    pilot.SetTargets(targets);
    pilot.SetCanCast(true);
    std::vector<uint32_t> castTicks;
    for (uint32_t t = 1; t <= 40; ++t) {
        const sim::PlayerInput in = pilot.Next(state);
        if (t == 1) {
            EXPECT_EQ(pilot.TargetSlot(), 2);
            const glm::vec3 eye = state.position + glm::vec3(0.0f, sim::PlayerTuning{}.eyeHeight, 0.0f);
            const glm::vec3 want = glm::normalize(glm::vec3(10.0f, 10.1f, 0.0f) - eye);
            EXPECT_GT(glm::dot(sim::LookDirection(in.yaw, in.pitch), want), std::cos(glm::radians(1.0f)));
        }
        if ((in.buttons & sim::PlayerButton_Cast) != 0) castTicks.push_back(t);
    }
    ASSERT_FALSE(castTicks.empty());
    EXPECT_GE(castTicks.front(), ps.reactionTicks);
    for (size_t i = 1; i < castTicks.size(); ++i) EXPECT_GT(castTicks[i], castTicks[i - 1] + 1) << "two ticks down in a row";
    pilot.SetCanCast(false);
    for (int t = 0; t < 30; ++t) EXPECT_EQ(pilot.Next(state).buttons & sim::PlayerButton_Cast, 0);
    EXPECT_EQ(pilot.FightCasts(), castTicks.size());
}
