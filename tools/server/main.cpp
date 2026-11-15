// vortex_server: the session server of docs/design/networking.md (stages 3-6).
//
//   vortex_server --session <file> [--listen [PORT]] [--bind ADDR] [--token HEX | --new-token]
//                 [--seconds S] [--exit-when-empty]
//                 [--ticks N] [--drill N] [--clients N] [--enet] [--join-step K]
//                 [--loss P] [--delay-ms D] [--jitter-ms J] [--outage-ms A,B] [--threads T]
//                 [--stand] [--no-jumps] [--bot-casts N] [--bot-crates N] [--realtime] [--hash-out <file>]
//
// Starts the server from a session file (token, region, template; see sessions/*.session), loads its
// whole window and applies the template.
//
// Network (stage 5): --listen [PORT] (default 27015) takes clients over UDP (ENet) on --bind ADDR
// (default every interface): game clients (`Shredborne --connect HOST[:PORT] --token HEX`) or other
// tools. The server then runs on the wall clock until --seconds S have passed, until every client
// that joined has left (--exit-when-empty), or until Ctrl-C; every client that leaves is logged with
// the server's opSeq and world hash at that moment (a client logs its own at exit: equal when it had
// applied everything), and the server's last opSeq and hash are logged when it ends. A port that
// cannot be opened ends it with exit code 2. The token is the session file's unless --token HEX
// gives another or --new-token makes a random one (printed at start); a server that listens on more
// than the loopback with the file's token warns loudly: the example session files are in the
// repository, so their tokens keep nobody out.
//
// In-process bots: N clients (--clients N; the first joins at once, the others after step K of the
// drill) each with its own sim::Simulation built from the server's Welcome. By default they talk to
// the server through LoopbackTransport + ImpairedTransport on a simulated clock with the given loss,
// delay and jitter. With --enet (implied by --listen) each bot opens its own ENet socket to the
// server's UDP port on 127.0.0.1 and everything runs on the wall clock; --loss is then real datagram
// loss below ENet (each socket drops that fraction of what it receives; ENet resends), delay and
// jitter stay emulated on the sending side. --drill N runs N server-side blasts that free terrain
// (net::BlastDrill: in-box islands with crumbs, face-flood islands, weak-neck releases), one every 6
// ticks. After the ticks (default: until the drill is done) the clients drain the stream and the world
// hashes are compared.
// Every client is a player (stage 4): a bot pilot (net::BotPilot) walks, runs, strafes and jumps
// it through the session (--stand: it stands still; --no-jumps), and with --bot-casts N it casts at
// the ground ahead every N ticks (the server captures the cast at its input tick and casts the ray
// when the cast runs, stage 5); each client runs on its own clock (TicksDue), predicts its player
// and is corrected by the snapshots. The report gives every client's correction statistics.
// Stage 6: the drill's pieces (terrain that breaks off, of the replicated size) are server bodies with
// a NetId, its crates (one beside each structure, in its blast; one per site in the margin) too, and
// with --bot-crates N every bot drops a crate (the game's E key) every N ticks. After the drain (which
// also waits, at most 10 s, until no replicated body moves) the entity check compares every bot's
// copies with the server's objects: the same NetIds and models, and those at rest within 5 cm.
// A server that fails (networking.md: a broken server world ends the session) stops at once and
// reports. Exit code 0 when every bot's hash equals the server's, every entity check agrees and
// nothing failed; 1 otherwise; 2 on a bad command line, session file or port.

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <random>
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

    /// Ctrl-C / SIGTERM: the server stops, reports and exits; a second one kills it (the handler
    /// puts the default back).
    std::atomic<bool> g_Stop{false};
    void OnSignal(int signal) {
        g_Stop = true;
        std::signal(signal, SIG_DFL);
    }

    /// A random session token (std::random_device: the kernel's generator on Linux).
    net::Token NewToken() {
        std::random_device device;
        net::Token t{};
        for (size_t i = 0; i < t.size(); i += 4) {
            const uint32_t v = device();
            for (size_t k = 0; k < 4 && i + k < t.size(); ++k) t[i + k] = static_cast<uint8_t>(v >> (8 * k));
        }
        return t;
    }

    bool IsLoopback(const std::string& address) { return address.rfind("127.", 0) == 0; }

    struct BotClient {
        std::unique_ptr<net::LoopbackTransport> loop;
        std::unique_ptr<net::EnetTransport> udp;   ///< --enet: the bot's own UDP socket
        std::unique_ptr<net::ImpairedTransport> link;
        std::unique_ptr<net::Client> client;
        std::unique_ptr<sim::Simulation> simulation;
        std::unique_ptr<net::BotPilot> pilot;
        bool connected = false;
        uint32_t joinStep = 0;
        uint32_t casts = 0;               ///< casts its prediction showed
        net::ClientSettings settings;
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
        std::fprintf(stderr, "usage: vortex_server --session <file> [--listen [PORT]] [--bind ADDR] [--seconds S] [--exit-when-empty] "
                             "[--token HEX | --new-token] [--ticks N] [--drill N] [--clients N] [--enet] [--join-step K] [--loss P] "
                             "[--delay-ms D] [--jitter-ms J] "
                             "[--outage-ms A,B] [--threads T] [--stand] [--no-jumps] [--bot-casts N] [--bot-crates N] [--realtime] "
                             "[--hash-out <file>]\n");
        return 2;
    }
    net::SessionConfig session;
    std::string error;
    if (!net::LoadSessionFile(*sessionPath, session, error)) {
        Log::Error("{}", error);
        return 2;
    }
    // The token: the file's, or one given or made here (review of stage 5: the example files are
    // checked in, so their tokens are public).
    bool fileToken = true;
    if (const std::optional<std::string> hex = args.Get("--token")) {
        if (!net::ParseToken(*hex, session.token)) {
            Log::Error("--token expects 32 hex digits: '{}'", *hex);
            return 2;
        }
        fileToken = false;
    } else if (args.Has("--new-token")) {
        session.token = NewToken();
        fileToken = false;
        Log::Info("Server: new session token {} (players: --token {})", net::FormatToken(session.token), net::FormatToken(session.token));
    }

    // --listen [PORT]: a UDP port for clients from elsewhere (stage 5); --enet: the bots use UDP too.
    const bool listen = args.Has("--listen");
    uint16_t port = net::kDefaultPort;
    if (const std::optional<std::string> p = args.Get("--listen")) {
        std::string host;
        if (!net::ParseEndpoint("x:" + *p, net::kDefaultPort, host, port)) {
            Log::Error("--listen expects a UDP port (1-65535): '{}'", *p);
            return 2;
        }
    }
    const bool enet = listen || args.Has("--enet");
    const double seconds = Number(args, "--seconds", 0.0);
    const bool exitWhenEmpty = args.Has("--exit-when-empty");

    const uint32_t drillSteps = static_cast<uint32_t>(Number(args, "--drill", 0));
    const uint32_t clientCount = static_cast<uint32_t>(Number(args, "--clients", drillSteps > 0 && !listen ? 2 : 0));
    const uint32_t joinStep = static_cast<uint32_t>(Number(args, "--join-step", drillSteps / 2));
    constexpr uint32_t kStepTicks = 6;
    // A listening server without a drill or bots runs until --seconds, --exit-when-empty or Ctrl-C.
    const bool openEnded = listen && drillSteps == 0 && clientCount == 0 && !args.Has("--ticks");
    const uint32_t ticks = static_cast<uint32_t>(Number(args, "--ticks", drillSteps > 0 ? drillSteps * kStepTicks : 120));
    const unsigned threads = static_cast<unsigned>(Number(args, "--threads", 0));
    const bool realtime = enet || args.Has("--realtime");
    const bool stand = args.Has("--stand");
    net::PilotSettings pilotSettings;
    pilotSettings.jumps = !args.Has("--no-jumps");
    pilotSettings.castEveryTicks = static_cast<uint32_t>(Number(args, "--bot-casts", 0));
    pilotSettings.casts = pilotSettings.castEveryTicks > 0;
    pilotSettings.crateEveryTicks = static_cast<uint32_t>(Number(args, "--bot-crates", 0));

    net::Impairment impairment;
    impairment.loss = Number(args, "--loss", 0.0);
    impairment.delaySeconds = Number(args, "--delay-ms", 0.0) / 1000.0;
    impairment.jitterSeconds = Number(args, "--jitter-ms", 0.0) / 1000.0;
    impairment.duplicate = impairment.loss > 0.0 ? 0.01 : 0.0;
    impairment.reorder = impairment.loss > 0.0 ? 0.02 : 0.0;
    // Over UDP the loss is real: each socket drops that fraction of the datagrams it receives and
    // ENet resends; the emulation above ENet keeps the delay, jitter, duplicates and reordering.
    const double udpLoss = enet ? impairment.loss : 0.0;
    if (enet) impairment.loss = 0.0;
    if (const std::optional<std::string> outage = args.Get("--outage-ms")) {
        double a = 0.0, b = 0.0;
        if (std::sscanf(outage->c_str(), "%lf,%lf", &a, &b) == 2 && b > a) impairment.outages.push_back({a / 1000.0, b / 1000.0});
    }

    // The clock every transport reads: simulated time, one tick per step (loopback), or the wall clock
    // (--realtime, and always over UDP: ENet's resend timers are real time).
    double clock = 0.0;
    const auto wallStart = std::chrono::steady_clock::now();
    auto wallNow = [wallStart] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count(); };
    auto now = [&clock, enet, wallNow] { return enet ? wallNow() : clock; };

    net::LoopbackTransport serverLoop;
    std::unique_ptr<net::EnetTransport> serverUdp;
    std::string bind;
    if (enet) {
        net::EnetSettings es;
        es.maxPeers = std::max<uint32_t>(session.maxClients + 8, clientCount + 8);   // room for peers that are refused
        es.receiveLoss = udpLoss;
        es.lossSeed = 0x5e7e7u;
        bind = listen ? args.GetString("--bind", "0.0.0.0") : std::string("127.0.0.1");
        std::string why;
        serverUdp = net::EnetTransport::Listen(bind, listen ? port : 0, es, &why);
        if (!serverUdp) {
            Log::Error("Server: {}", why);
            return 2;
        }
        port = serverUdp->Port();
        Log::Info("Server: listening on UDP {}:{} (ENet){}", bind, port, udpLoss > 0.0 ? std::format(", dropping {:.1f} % of what it receives", udpLoss * 100.0) : "");
        if (listen && fileToken && !IsLoopback(bind))
            Log::Warn("Server: this server is reachable on {} with the token of {}. The example session files are in the repository: "
                      "anyone with it can join and cast. Start it with --new-token (and give the printed token to the players), "
                      "or put a fresh token into the file (`openssl rand -hex 16`), or --bind 127.0.0.1 for this machine only.",
                      bind == "0.0.0.0" ? std::string("every interface") : bind, *sessionPath);
    }
    net::ITransport& serverBase = enet ? static_cast<net::ITransport&>(*serverUdp) : static_cast<net::ITransport&>(serverLoop);
    net::ImpairedTransport serverLink(serverBase, impairment, 0x5e7e7u, now);
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
        net::ClientSettings cs;
        cs.token = session.token;
        cs.presentation = i == 0;          // the first client also makes particles and rubble, as a game would
        bot.joinStep = i == 0 ? 0 : joinStep;
        bot.settings = cs;
        bot.pilot = std::make_unique<net::BotPilot>(server.Region(), 0xb07u + i, pilotSettings);
    }

    std::unique_ptr<net::BlastDrill> drill;
    if (drillSteps > 0) {
        drill = std::make_unique<net::BlastDrill>(server.Region(), server.Simulation().WorldMaterialIds(), server.Simulation().World());
        Log::Info("Drill: {} steps on {} sites, one every {} ticks", drillSteps, drill->Sites(), kStepTicks);
    }

    bool draining = false;                       // the drain: the players stand (no more casts)
    double passSeconds = dt;                     // time of the current pass (one tick, or the wall clock's)
    // Over UDP a bot that loads its region keeps the rest running (the server, the other bots): the
    // load's callback runs whole passes without it. On the loopback the clock is simulated and the
    // load stays outside it, as before.
    const BotClient* loading = nullptr;
    std::function<void()> nestedPass;
    auto stepClients = [&](uint32_t step) {
        for (BotClient& bot : bots) {
            if (&bot == loading) continue;
            if (loading != nullptr && !bot.connected) continue;   // no new joins inside a load
            if (!bot.connected && step >= bot.joinStep) {
                const size_t i = static_cast<size_t>(&bot - bots.data());
                net::ITransport* base = bot.loop.get();
                if (enet) {
                    net::EnetSettings es;
                    es.receiveLoss = udpLoss;
                    es.lossSeed = 0xc1e47u + i;
                    std::string why;
                    // The bots reach the server where it listens (every interface: through 127.0.0.1).
                    const std::string botHost = bind.empty() || bind == "0.0.0.0" ? std::string("127.0.0.1") : bind;
                    bot.udp = net::EnetTransport::Connect(botHost, port, es, &why);
                    if (!bot.udp) {
                        Log::Error("Client {}: {}", i, why);
                        continue;
                    }
                    base = bot.udp.get();
                } else {
                    net::LoopbackTransport::Connect(serverLoop, *bot.loop);
                }
                bot.link = std::make_unique<net::ImpairedTransport>(*base, impairment, 0xc1e47u + i, now);
                bot.client = std::make_unique<net::Client>(bot.settings, *bot.link);
                bot.connected = true;
            }
            if (!bot.connected) continue;
            if (bot.client->State() == net::ClientState::Welcomed && !bot.simulation && loading == nullptr) {
                sim::SimulationConfig base;
                base.jobThreads = std::max(1u, threads / 2);
                base.physics.threads = 1;
                if (session.chunkCapacity != 0) base.chunkCapacity = session.chunkCapacity;   // else the region's (MakeSimulationConfig)
                bot.simulation = std::make_unique<sim::Simulation>(bot.client->MakeSimulationConfig(base));
                loading = &bot;
                const double t0 = wallNow();
                const bool initialized = bot.simulation->Initialize(bot.client->MakeGenerator());
                const double t1 = wallNow();
                const bool attached = initialized && bot.client->Attach(*bot.simulation, [&bot, &nestedPass, enet] {
                                          bot.client->Poll();
                                          bot.link->Flush();
                                          if (enet) nestedPass();
                                      });
                // Over UDP the other participants stand still during the initialisation (one thread).
                Log::Info("Client {}: simulation initialised in {:.3f} s, region loaded and attached in {:.3f} s",
                          static_cast<size_t>(&bot - bots.data()), t1 - t0, wallNow() - t1);
                // The rest of the load (after its last callback: the final checks) for everyone else
                // too; the passes inside the load have given them the time before.
                if (enet) {
                    nestedPass();
                    passSeconds = 0.0;
                }
                loading = nullptr;
                if (!attached) {
                    Log::Error("Client: could not attach a simulation");
                    bot.simulation.reset();
                }
            }
            // The client's own clock (kept ahead of the server) says how many ticks it runs now.
            const uint32_t due = bot.client->TicksDue(passSeconds);
            for (uint32_t k = 0; k < due; ++k) {
                const sim::PlayerInput controls =
                    bot.client->Predicting() && !stand && !draining ? bot.pilot->Next(bot.client->Player()) : sim::PlayerInput{};
                const sim::PlayerStepResult r = bot.client->RunTick(controls);
                bot.casts += r.cast ? 1u : 0u;
                if (bot.simulation)
                    for (const sim::BlastResult& blast : bot.client->TakePresentations()) sim::PresentBlast(*bot.simulation, blast);
            }
            bot.link->Flush();
        }
    };

    uint32_t step = 0;
    uint32_t drilledTick = 0xFFFFFFFFu;          // a pass that runs no tick must not queue the step again
    auto drillStep = [&](uint32_t t) {
        if (drill && t % kStepTicks == 0 && step < drillSteps && t != drilledTick) {
            drilledTick = t;
            const net::DrillStep s = drill->Step(step);
            if (!net::RunDrillStep(server, s)) Log::Warn("Drill: step {} cast outside the play bounds", step);
            ++step;
        }
    };
    // Clients that left a listening server: logged with the server's opSeq and hash at that moment.
    size_t lastClients = 0, mostClients = 0;
    uint32_t lastReported = 0;
    auto watchClients = [&] {
        const size_t n = server.ClientCount();
        if (n < lastClients) {
            const voxel::WorldHashResult h = server.Hash();
            Log::Info("Session: {} client(s) left; server at opSeq {}, tick {}, world hash {:016x} ({} chunks)", lastClients - n,
                      server.LastOpSeq(), server.TickNumber(), h.hash, h.solidChunks);
        }
        mostClients = std::max(mostClients, n);
        lastClients = n;
    };
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    double lastWall = 0.0;
    // One pass: the server's due ticks (one simulated tick, or what the wall clock gives), then the bots.
    auto pass = [&] {
        if (enet) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const double t = wallNow();
            passSeconds = t - lastWall;
            lastWall = t;
            const uint32_t before = server.TickNumber();
            // Drill steps are queued between ticks, on the tick they are due (Advance runs at most 2).
            drillStep(before);
            server.Advance(passSeconds);
            if (server.TickNumber() > before + 1) drillStep(before + 1);
        } else {
            drillStep(server.TickNumber());
            server.Tick();
            clock += dt;
            passSeconds = dt;
        }
        serverLink.Flush();
        stepClients(step);
        if (listen) {
            watchClients();
            // A status line every 10 s of a listening server.
            if (server.TickNumber() / 600 != lastReported) {
                lastReported = server.TickNumber() / 600;
                const net::ServerStats& st = server.Stats();
                // The event log a joiner receives (until stage 7) grows for the whole session.
                Log::Info("Server: tick {}, {} client(s), opSeq {}, event log {:.1f} KiB (joins refused above {:.0f} MiB), {} casts run ({} "
                          "from players), {} snapshots sent",
                          server.TickNumber(), server.ClientCount(), server.LastOpSeq(), static_cast<double>(server.LogBytes()) / 1024.0,
                          static_cast<double>(settings.maxJoinLogBytes) / 1048576.0, st.castsRun, st.castsFromInputs, st.snapshots);
            }
        }
        if (!enet && realtime) std::this_thread::sleep_until(wallStart + std::chrono::duration<double>(clock));
    };
    nestedPass = pass;

    if (openEnded) {
        Log::Info("Server: waiting for clients ({}); {}", fileToken ? "token in " + *sessionPath : std::string("token given at start"),
                  seconds > 0.0 ? std::format("stops after {:.0f} s", seconds)
                                : exitWhenEmpty ? std::string("stops when every client has left") : std::string("Ctrl-C stops it"));
        while (!g_Stop && !server.Failed()) {
            pass();
            if (seconds > 0.0 && wallNow() >= seconds) break;
            if (exitWhenEmpty && mostClients > 0 && server.ClientCount() == 0) break;
        }
    } else {
        while (server.TickNumber() < ticks && !g_Stop) {
            pass();
            if (server.Failed()) break;          // the session has ended (logged); report and exit 1
        }
    }
    // Drain: the players stop, the queue empties and every client catches up (at most 10 s of time).
    draining = true;
    uint32_t drain = 0;
    auto settled = [&] {
        if (server.QueuedCasts() > 0) return false;
        // Stage 6: the replicated bodies come to rest too (the entity check compares rest poses).
        if (server.AwakeBodies() > 0) return false;
        for (const BotClient& bot : bots)
            if (!bot.simulation || bot.client->AppliedOpSeq() != server.LastOpSeq()) return false;
        return true;
    };
    const uint32_t drainStart = server.TickNumber();
    // Ctrl-C skips it (and fails the run: the hashes were not compared after a drain).
    while (!openEnded && !g_Stop && !server.Failed() && !settled() && server.TickNumber() - drainStart < 600) {
        pass();
        drain = server.TickNumber() - drainStart;
    }
    const bool interrupted = g_Stop && !openEnded;
    if (interrupted) Log::Warn("Server: stopped by a signal before the run ended: no drain, the run fails");
    const double wall = wallNow();

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
              "stood), {} held; {} casts from inputs ({} from late inputs, {} refused by the cooldown, {} met no terrain at dequeue); {} snapshots, {:.1f} B each",
              st.inputPackets, st.malformedInputs, st.inputsQueued, st.inputsLate, st.inputsDuplicate, st.inputsTooEarly, st.ticksRepeated,
              st.ticksStood, st.heldTicks, st.castsFromInputs, st.castsFromLateInputs, st.castsOnCooldown, st.castsMissed, st.snapshots,
              st.snapshots ? static_cast<double>(st.snapshotBytes) / static_cast<double>(st.snapshots) : 0.0);
    const double tickSeconds = static_cast<double>(server.Simulation().FixedTimeStep());
    for (size_t i = 0; i < bots.size(); ++i) {
        if (!bots[i].client) continue;           // never connected (reported below)
        const net::PredictionStats& p = bots[i].client->Prediction();
        Log::Info("Client {} movement: {} ticks predicted ({} held), {} snapshots compared: {} within 2 cm, {} corrected ({:.2f} per minute), "
                  "{} snapped, max error {:.3f} m; errors <1 mm {}, <2 cm {}, <5 cm {}, <20 cm {}, <1.5 m {}, more {}; replays {} ticks (max "
                  "{}, {:.3f} ms max); waited for events {}, gave up {}; clock {} jumps / {} resyncs, margin {}..{}, target {}; {} casts "
                  "shown ({} the server did not run), pilot cast {} / jumped {}",
                  i, p.ticks, p.heldTicks, p.compared, p.within, p.corrections, p.CorrectionsPerMinute(tickSeconds), p.snaps, p.maxError,
                  p.errorBuckets[0], p.errorBuckets[1], p.errorBuckets[2], p.errorBuckets[3], p.errorBuckets[4], p.errorBuckets[5],
                  p.replayTicks, p.maxReplayTicks, p.maxReplaySeconds * 1000.0, p.waitedForEvents, p.eventTimeouts, p.clockJumps,
                  p.hardResyncs, p.minMargin, p.maxMargin, bots[i].client->Clock().Target(), bots[i].casts, p.castsRejected, bots[i].pilot->Casts(),
                  bots[i].pilot->Jumps());
        const net::ClientStats& cs = bots[i].client->Stats();
        Log::Info("Client {} objects: {} copies made ({} failed), {} despawned ({} kept as rubble), {} rests, {} models, {} fragments; {} "
                  "body samples taken, {} ignored, largest sample gap {} ticks; pilot crates {}",
                  i, cs.copiesMade, cs.copiesFailed, cs.despawns, cs.toRubble, cs.rests, cs.models, cs.fragments, cs.bodySamples,
                  cs.bodySamplesIgnored, bots[i].client->Bodies().MaxGap(), bots[i].pilot->Crates());
    }
    {
        const sim::ReplicationStats& rs = server.Simulation().Replication().stats;
        Log::Info("Objects: {} server bodies from terrain pieces, {} object spawns, {} fragments as client rubble, {} models, {} rests, "
                  "despawns {} destroyed / {} to rubble / {} broken; pieces: {} replicated, {} over the tick budget, {} over the awake cap, {} "
                  "evicted ({} resting, {} awake), {} body failures, at most {} live / {} awake; crates {} ({} refused); entity events "
                  "{:.1f} KiB; snapshots up to {} B with {} bodies, {} bodies sent, {} left out",
                  st.detachBodies, st.objectSpawns, st.fragmentSpawns, st.objectModels, st.rests, st.despawns[1], st.despawns[2],
                  st.despawns[3], rs.pieces, rs.overTickBudget, rs.overAwakeCap, rs.evictedResting + rs.evictedAwake, rs.evictedResting,
                  rs.evictedAwake, rs.bodyFailures, rs.maxLive, rs.maxAwake, st.crates, st.cratesRefused,
                  static_cast<double>(st.entityEventBytes) / 1024.0, st.maxSnapshotBytes, st.maxSnapshotBodies, st.bodySamples, st.bodiesLeftOut);
    }
    const voxel::WorldStats ws = server.Simulation().World().Stats();
    Log::Info("Server: world {} dense / {} packed chunks, {:.1f} MiB RLE, store {} / {} slots, search errors {}, incomplete ops {}, "
              "RSS {:.0f} MiB (all simulations of this process)",
              ws.denseChunks, ws.packedChunks, static_cast<double>(ws.packedBytes) / (1024.0 * 1024.0),
              server.Simulation().Chunks().UsedCount(), server.Simulation().Chunks().Capacity(), st.searchErrors, st.incompleteOps,
              ResidentMiB());

    const voxel::WorldHashResult sh = server.Hash();
    bool ok = !server.Failed() && sh.Complete() && !interrupted;
    // The entity check (stage 6): the same NetIds and models on every side, rest poses within 5 cm.
    const std::vector<net::EntityInfo> serverEntities = server.Entities();
    Log::Info("Entities: server {} object(s), {} awake, id hash {:016x}", serverEntities.size(), server.AwakeBodies(),
              net::EntityListHash(serverEntities));
    for (size_t i = 0; i < bots.size(); ++i) {
        if (!bots[i].simulation) continue;
        const net::EntityComparison c = net::CompareEntities(serverEntities, bots[i].client->Entities());
        const bool agree = c.Agree(0.05f);
        ok = ok && agree;
        Log::Info("Entities: client {}: {} - {}", i, c.Describe(), agree ? "agree" : "DIFFERENT");
    }
    // What every client that drained must have reached (tools/session-check.sh compares against it).
    Log::Info("Session: server ends at opSeq {}, tick {}, world hash {:016x} ({} chunks)", server.LastOpSeq(), server.TickNumber(), sh.hash,
              sh.solidChunks);
    std::string hashes = std::format("server {:016x} ({} chunks)", sh.hash, sh.solidChunks);
    for (size_t i = 0; i < bots.size(); ++i) {
        const BotClient& bot = bots[i];
        if (!bot.client) {
            Log::Error("Client {}: never connected", i);
            ok = false;
            continue;
        }
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
        if (bot.udp)
            Log::Info("Client {} UDP: {} of {} datagrams dropped on receive, RTT {:.1f} ms, {} messages sent / {} received", i,
                      bot.udp->Stats().datagramsDropped, bot.udp->Stats().datagramsReceived, bot.udp->RoundTripSeconds(1) * 1000.0,
                      bot.udp->Stats().messagesSent, bot.udp->Stats().messagesReceived);
    }
    if (serverUdp)
        Log::Info("Server UDP: {} of {} datagrams dropped on receive, {} messages sent / {} received, {:.1f} KiB sent", serverUdp->Stats().datagramsDropped,
                  serverUdp->Stats().datagramsReceived, serverUdp->Stats().messagesSent, serverUdp->Stats().messagesReceived,
                  static_cast<double>(serverUdp->Stats().bytesSent) / 1024.0);
    if (const std::optional<std::string> out = args.Get("--hash-out")) {
        std::ofstream file(*out);
        file << hashes << "\n";
    }
    Log::Info("Session: {} ({})", ok ? "hashes equal" : "FAILED", hashes);
    return ok ? 0 : 1;
}
