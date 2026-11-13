// vortex_server: the session server of docs/design/networking.md (stages 3-4: loopback only).
//
//   vortex_server --session <file> [--ticks N] [--drill N] [--clients N] [--join-step K]
//                 [--loss P] [--delay-ms D] [--jitter-ms J] [--outage-ms A,B] [--threads T]
//                 [--stand] [--no-jumps] [--bot-casts N] [--realtime] [--hash-out <file>]
//
// Starts the server from a session file (token, region, template; see sessions/*.session), loads its
// whole window and applies the template. Until a network transport exists (stage 5) the clients
// are in-process: N clients (the first joins at once, the others after step K of the drill) talk
// to the server through LoopbackTransport + ImpairedTransport with the given loss, delay and
// jitter, each with its own sim::Simulation built from the server's Welcome. --drill N runs N
// server-side blasts that free terrain (net::BlastDrill: in-box islands with crumbs, face-flood
// islands, weak-neck releases), one every 6 ticks. After the ticks (default: until the drill is
// done) the clients drain the stream and the world hashes are compared.
// Every client is a player (stage 4): a bot pilot (net::BotPilot) walks, runs, strafes and jumps
// it through the session (--stand: it stands still; --no-jumps), and with --bot-casts N it casts at
// the ground ahead every N ticks; each client runs on its own clock (TicksDue), predicts its player
// and is corrected by the snapshots. The report gives every client's correction statistics.
// A server that fails (networking.md: a broken server world ends the session) stops at once and
// reports. Exit code 0 when every client's hash equals the server's and nothing failed; 1 otherwise;
// 2 on a bad command line or session file.

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.scene;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    /// Resident memory of this process in MiB (Linux /proc; 0 elsewhere).
    double ResidentMiB() {
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.rfind("VmRSS:", 0) == 0) return std::atof(line.c_str() + 6) / 1024.0;
        }
        return 0.0;
    }

    struct BotClient {
        std::unique_ptr<net::LoopbackTransport> loop;
        std::unique_ptr<net::ImpairedTransport> link;
        std::unique_ptr<net::Client> client;
        std::unique_ptr<sim::Simulation> simulation;
        std::unique_ptr<net::BotPilot> pilot;
        bool connected = false;
        uint32_t joinStep = 0;
        uint32_t casts = 0;               ///< casts its prediction showed
    };

    double Number(const CommandLine& args, const char* name, double fallback) {
        const std::optional<std::string> v = args.Get(name);
        return v ? std::atof(v->c_str()) : fallback;
    }
}

int main(int argc, char** argv) {
    Log::Init();
    const CommandLine args(argc, argv);
    const std::optional<std::string> sessionPath = args.Get("--session");
    if (!sessionPath) {
        std::fprintf(stderr, "usage: vortex_server --session <file> [--ticks N] [--drill N] [--clients N] [--join-step K] [--loss P] "
                             "[--delay-ms D] [--jitter-ms J] [--outage-ms A,B] [--threads T] [--stand] [--no-jumps] [--bot-casts N] "
                             "[--realtime] [--hash-out <file>]\n");
        return 2;
    }
    net::SessionConfig session;
    std::string error;
    if (!net::LoadSessionFile(*sessionPath, session, error)) {
        Log::Error("{}", error);
        return 2;
    }

    const uint32_t drillSteps = static_cast<uint32_t>(Number(args, "--drill", 0));
    const uint32_t clientCount = static_cast<uint32_t>(Number(args, "--clients", drillSteps > 0 ? 2 : 0));
    const uint32_t joinStep = static_cast<uint32_t>(Number(args, "--join-step", drillSteps / 2));
    constexpr uint32_t kStepTicks = 6;
    const uint32_t ticks = static_cast<uint32_t>(Number(args, "--ticks", drillSteps > 0 ? drillSteps * kStepTicks : 120));
    const unsigned threads = static_cast<unsigned>(Number(args, "--threads", 0));
    const bool realtime = args.Has("--realtime");
    const bool stand = args.Has("--stand");
    net::PilotSettings pilotSettings;
    pilotSettings.jumps = !args.Has("--no-jumps");
    pilotSettings.castEveryTicks = static_cast<uint32_t>(Number(args, "--bot-casts", 0));
    pilotSettings.casts = pilotSettings.castEveryTicks > 0;

    net::Impairment impairment;
    impairment.loss = Number(args, "--loss", 0.0);
    impairment.delaySeconds = Number(args, "--delay-ms", 0.0) / 1000.0;
    impairment.jitterSeconds = Number(args, "--jitter-ms", 0.0) / 1000.0;
    impairment.duplicate = impairment.loss > 0.0 ? 0.01 : 0.0;
    impairment.reorder = impairment.loss > 0.0 ? 0.02 : 0.0;
    if (const std::optional<std::string> outage = args.Get("--outage-ms")) {
        double a = 0.0, b = 0.0;
        if (std::sscanf(outage->c_str(), "%lf,%lf", &a, &b) == 2 && b > a) impairment.outages.push_back({a / 1000.0, b / 1000.0});
    }

    // The clock every transport reads: simulated time, one tick per step (or wall clock with --realtime).
    double clock = 0.0;
    auto now = [&clock] { return clock; };

    net::LoopbackTransport serverLoop;
    net::ImpairedTransport serverLink(serverLoop, impairment, 0x5e7e7u, now);
    net::ServerSettings settings;
    settings.session = session;
    settings.simulation.jobThreads = threads;
    settings.castRecordLimit = std::max(settings.castRecordLimit, drillSteps);   // the drill report reads every cast
    net::Server server(settings, serverLink);
    if (!server.Start()) return 1;
    const double dt = static_cast<double>(server.Simulation().FixedTimeStep());

    std::vector<BotClient> bots(clientCount);
    for (uint32_t i = 0; i < clientCount; ++i) {
        BotClient& bot = bots[i];
        bot.loop = std::make_unique<net::LoopbackTransport>();
        bot.link = std::make_unique<net::ImpairedTransport>(*bot.loop, impairment, 0xc1e47u + i, now);
        net::ClientSettings cs;
        cs.token = session.token;
        cs.presentation = i == 0;          // the first client also makes particles and rubble, as a game would
        bot.client = std::make_unique<net::Client>(cs, *bot.link);
        bot.joinStep = i == 0 ? 0 : joinStep;
        bot.pilot = std::make_unique<net::BotPilot>(server.Region(), 0xb07u + i, pilotSettings);
    }

    std::unique_ptr<net::BlastDrill> drill;
    if (drillSteps > 0) {
        drill = std::make_unique<net::BlastDrill>(server.Region(), server.Simulation().WorldMaterialIds(), server.Simulation().World());
        Log::Info("Drill: {} steps on {} sites, one every {} ticks", drillSteps, drill->Sites(), kStepTicks);
    }

    bool draining = false;                       // the drain: the players stand (no more casts)
    auto stepClients = [&](uint32_t step) {
        for (BotClient& bot : bots) {
            if (!bot.connected && step >= bot.joinStep) {
                net::LoopbackTransport::Connect(serverLoop, *bot.loop);
                bot.connected = true;
            }
            if (!bot.connected) continue;
            if (bot.client->State() == net::ClientState::Welcomed && !bot.simulation) {
                sim::SimulationConfig base;
                base.jobThreads = std::max(1u, threads / 2);
                base.physics.threads = 1;
                if (session.chunkCapacity != 0) base.chunkCapacity = session.chunkCapacity;   // else the region's (MakeSimulationConfig)
                bot.simulation = std::make_unique<sim::Simulation>(bot.client->MakeSimulationConfig(base));
                if (!bot.simulation->Initialize(bot.client->MakeGenerator()) || !bot.client->Attach(*bot.simulation)) {
                    Log::Error("Client: could not attach a simulation");
                    bot.simulation.reset();
                }
            }
            // The client's own clock (kept ahead of the server) says how many ticks it runs now.
            const uint32_t due = bot.client->TicksDue(dt);
            for (uint32_t k = 0; k < due; ++k) {
                const sim::PlayerInput controls =
                    bot.client->Predicting() && !stand && !draining ? bot.pilot->Next(bot.client->Player()) : sim::PlayerInput{};
                const sim::PlayerStepResult r = bot.client->RunTick(controls);
                bot.casts += r.cast ? 1u : 0u;
                if (bot.simulation)
                    for (const sim::BlastResult& blast : bot.client->TakePresentations()) sim::PresentBlast(*bot.simulation, blast);
            }
        }
    };

    const auto wallStart = std::chrono::steady_clock::now();
    uint32_t step = 0;
    for (uint32_t t = 0; t < ticks; ++t) {
        if (drill && t % kStepTicks == 0 && step < drillSteps) {
            const net::DrillStep s = drill->Step(step);
            for (const sim::WorldOp& op : s.build) server.ApplyOp(op);
            if (!server.QueueCast(s.cast, s.power)) Log::Warn("Drill: step {} cast outside the play bounds", step);
            ++step;
        }
        server.Tick();
        clock += dt;
        stepClients(step);
        if (server.Failed()) break;              // the session has ended (logged); report and exit 1
        if (realtime) std::this_thread::sleep_until(wallStart + std::chrono::duration<double>(clock));
    }
    // Drain: the players stop, the queue empties and every client catches up (at most 10 s of simulated time).
    draining = true;
    uint32_t drain = 0;
    auto settled = [&] {
        if (server.QueuedCasts() > 0) return false;
        for (const BotClient& bot : bots)
            if (!bot.simulation || bot.client->AppliedOpSeq() != server.LastOpSeq()) return false;
        return true;
    };
    while (!server.Failed() && !settled() && drain < 600) {
        server.Tick();
        clock += dt;
        stepClients(step);
        ++drain;
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();

    // Report.
    const net::ServerStats& st = server.Stats();
    uint32_t kinds[3] = {0, 0, 0}, freed[3] = {0, 0, 0};
    uint32_t drillCast = 0;
    for (size_t i = 0; i < server.Casts().size(); ++i) {
        const net::CastRecord& c = server.Casts()[i];
        if (c.player != 0) continue;           // a player's cast, not a drill step
        const uint32_t index = drillCast++;
        if (drill && index < drillSteps) {
            const int k = static_cast<int>(drill->Step(index).kind);
            ++kinds[k];
            const bool ok = k == 0 ? (c.inBoxIslands > 0 && c.crumbs > 0) : k == 1 ? c.facePieces > 0 : c.neckPieces > 0;
            freed[k] += ok ? 1u : 0u;
        }
    }
    Log::Info("Server: {} ticks ({} drain) in {:.2f} s wall, {} events / {:.1f} KiB ({} over the burst), {} casts run ({} rejected, {} "
              "past the budgets), work {} total / {} max per cast, waited for work on {} ticks, for bytes on {} ticks",
              server.TickNumber(), drain, wall, st.events, static_cast<double>(st.eventBytes) / 1024.0, st.oversizeEvents, st.castsRun,
              st.castsRejected, st.castsOverdue, st.castWork, st.maxCastWork, st.ticksWaitedForWork, st.ticksWaitedForBytes);
    Log::Info("Server: cast delay in ticks 0..6, 7+: {} {} {} {} {} {} {} {} (max {})", st.castDelay[0], st.castDelay[1], st.castDelay[2],
              st.castDelay[3], st.castDelay[4], st.castDelay[5], st.castDelay[6], st.castDelay[7], st.maxCastDelay);
    // Wall clock of the casts: logged only (shred.md R5), to see a miscalibrated work unit (est. 1 ms per 10 000).
    Log::Info("Server: cast wall time {:.1f} ms in all, {:.2f} ms max per cast, {:.2f} ms per 10 000 work units overall, {:.2f} max per cast",
              st.castSeconds * 1000.0, st.maxCastSeconds * 1000.0,
              st.castWork > 0 ? st.castSeconds * 1000.0 * 10000.0 / static_cast<double>(st.castWork) : 0.0, st.maxCastMsPer10k);
    Log::Info("Server: event stream paced {} times (a peer's bytes in flight at the limit), largest backlog {} events; {} clients joined, "
              "{} left, {} refused, {} peers timed out",
              st.sendsHeld, st.maxBacklogEvents, st.clientsJoined, st.clientsDisconnected, st.clientsRefused, st.peersTimedOut);
    if (drill)
        Log::Info("Drill: freed terrain in {}/{} mushroom, {}/{} bar, {}/{} neck-tower steps: {} Detach ({} in-box islands, {} face-flood "
                  "islands, {} neck releases), {} crumbs",
                  freed[0], kinds[0], freed[1], kinds[1], freed[2], kinds[2], st.islands, st.inBoxIslands, st.facePieces, st.neckPieces,
                  st.crumbs);
    Log::Info("Players: {} input packets ({} malformed), inputs {} queued / {} late / {} duplicate / {} too early; {} ticks repeated ({} "
              "stood), {} held; {} casts from inputs ({} from late inputs, {} met no terrain); {} snapshots, {:.1f} B each",
              st.inputPackets, st.malformedInputs, st.inputsQueued, st.inputsLate, st.inputsDuplicate, st.inputsTooEarly, st.ticksRepeated,
              st.ticksStood, st.heldTicks, st.castsFromInputs, st.castsFromLateInputs, st.castsMissed, st.snapshots,
              st.snapshots ? static_cast<double>(st.snapshotBytes) / static_cast<double>(st.snapshots) : 0.0);
    const double tickSeconds = static_cast<double>(server.Simulation().FixedTimeStep());
    for (size_t i = 0; i < bots.size(); ++i) {
        const net::PredictionStats& p = bots[i].client->Prediction();
        Log::Info("Client {} movement: {} ticks predicted ({} held), {} snapshots compared: {} within 2 cm, {} corrected ({:.2f} per minute), "
                  "{} snapped, max error {:.3f} m; errors <1 mm {}, <2 cm {}, <5 cm {}, <20 cm {}, <1.5 m {}, more {}; replays {} ticks (max "
                  "{}, {:.3f} ms max); waited for events {}, gave up {}; clock {} jumps / {} resyncs, margin {}..{}, target {}; {} casts "
                  "shown, pilot cast {} / jumped {}",
                  i, p.ticks, p.heldTicks, p.compared, p.within, p.corrections, p.CorrectionsPerMinute(tickSeconds), p.snaps, p.maxError,
                  p.errorBuckets[0], p.errorBuckets[1], p.errorBuckets[2], p.errorBuckets[3], p.errorBuckets[4], p.errorBuckets[5],
                  p.replayTicks, p.maxReplayTicks, p.maxReplaySeconds * 1000.0, p.waitedForEvents, p.eventTimeouts, p.clockJumps,
                  p.hardResyncs, p.minMargin, p.maxMargin, bots[i].client->Clock().Target(), bots[i].casts, bots[i].pilot->Casts(),
                  bots[i].pilot->Jumps());
    }
    const voxel::WorldStats ws = server.Simulation().World().Stats();
    Log::Info("Server: world {} dense / {} packed chunks, {:.1f} MiB RLE, store {} / {} slots, search errors {}, incomplete ops {}, "
              "RSS {:.0f} MiB (all simulations of this process)",
              ws.denseChunks, ws.packedChunks, static_cast<double>(ws.packedBytes) / (1024.0 * 1024.0),
              server.Simulation().Chunks().UsedCount(), server.Simulation().Chunks().Capacity(), st.searchErrors, st.incompleteOps,
              ResidentMiB());

    const voxel::WorldHashResult sh = server.Hash();
    bool ok = !server.Failed() && sh.Complete();
    std::string hashes = std::format("server {:016x} ({} chunks)", sh.hash, sh.solidChunks);
    for (size_t i = 0; i < bots.size(); ++i) {
        const BotClient& bot = bots[i];
        const voxel::WorldHashResult ch = bot.client->Hash();
        const net::ClientStats& cs = bot.client->Stats();
        const bool equal = bot.simulation && ch.Complete() && ch.hash == sh.hash && bot.client->AppliedOpSeq() == server.LastOpSeq();
        ok = ok && equal && cs.skippedChunks == 0 && cs.unsyncedChunks == 0 && cs.probeMismatches == 0;
        Log::Info("Client {}: {} at opSeq {} / {}, log at join {}, {} events / {:.1f} KiB, {} duplicates, {} probe mismatches, {} skipped / "
                  "{} unsynced chunks, hash {:016x} {}",
                  i, bot.simulation ? "attached" : "not attached", bot.client->AppliedOpSeq(), server.LastOpSeq(), bot.client->LogEnd(),
                  cs.eventsApplied, static_cast<double>(cs.eventBytes) / 1024.0, cs.duplicateEvents, cs.probeMismatches, cs.skippedChunks,
                  cs.unsyncedChunks, ch.hash, equal ? "equal" : "DIFFERENT");
        hashes += std::format(", client {} {:016x}", i, ch.hash);
    }
    if (const std::optional<std::string> out = args.Get("--hash-out")) {
        std::ofstream file(*out);
        file << hashes << "\n";
    }
    Log::Info("Session: {} ({})", ok ? "hashes equal" : "FAILED", hashes);
    return ok ? 0 : 1;
}
