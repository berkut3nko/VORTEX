// vortex_server: the session server of docs/design/networking.md (stages 3-6).
//
//   vortex_server --session <file> [--listen [PORT]] [--bind ADDR] [--token HEX | --new-token]
//                 [--seconds S] [--exit-when-empty]
//                 [--ticks N] [--drill N] [--clients N] [--enet] [--join-step K]
//                 [--loss P] [--delay-ms D] [--jitter-ms J] [--outage-ms A,B] [--threads T]
//                 [--stand] [--no-jumps] [--bot-casts N] [--bot-crates N] [--realtime] [--hash-out <file>]
//                 [--drill-after N] [--drill-delay S] [--drill-sites K] [--drill-every T]
//                 [--client-window N] [--walk-across] [--precasts N] [--memory] [--max-join-p99-ticks N]
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
// --drill-every T runs a drill step every T ticks instead of 6. --drill-after N starts the drill only once N clients have joined (and --drill-delay S seconds more):
// a listening server then runs it beside a session with game clients and keeps running (until
// --seconds, --exit-when-empty or Ctrl-C); --drill-sites K uses only the first K sites (the play
// bounds' -x / -z corner, away from a template's middle). A listening server logs the NetIds and
// models of its objects (an entity list hash) next to its world hash whenever a client leaves and at
// the end (tools/session-check.sh compares them with the game clients').
// Stage 7a: joiners are synced chunk by chunk (the modified chunks of their window and the entity
// baseline on paced bulk; `Join:` lines on both sides). --client-window N gives the bots a window of N
// chunks (the game's is 48): on a region wider than that it follows the bot's player (M-b), the bots get
// the game's store (12 288 slots, 2048 kept free) and the session's packing, and the run fails if a bot's
// store fills or keeps fewer than 2048 free. --walk-across makes bot 0 run from the play bounds' -x
// edge to the +x edge along the middle z and back to the middle (the run lasts until it is done);
// --precasts N first runs N server casts spread along that line, before any bot joins, so the walker
// walks into chunks modified before. --memory logs the process's resident memory and the server
// world's chunk counts every 10 s of ticks and at the end. A bot's hash is compared with the
// server's over its sync box (its window inside the server's) at the same opSeq, once it is synced. Each bot also
// logs `Latency: bot N:` - server ticks from an event being made to the bot applying it, p99 and max for the events
// made while some bot was joining (connected until its join data was in) and for all events;
// --max-join-p99-ticks N makes a p99 of N ticks or more during the joins fail the run: it must be under N, as in the
// unit test (review of stage 7a; recheck 6: 15 = 250 ms).
// A server that fails (networking.md: a broken server world ends the session) stops at once and
// reports. Exit code 0 when every bot's hash equals the server's, every entity check agrees and
// nothing failed; 1 otherwise; 2 on a bad command line, session file or port.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
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
        float minX = 1e30f, maxX = -1e30f;   ///< how far its player went (x, metres)
        net::ClientSettings settings;
        // Event latency (stage 7a): server ticks from an event being made to this bot applying it.
        uint32_t seen = 0;                ///< the opSeq applied up to, as last measured
        bool counting = false;            ///< attached: its events are measured (from its join point on)
        std::vector<std::pair<uint32_t, uint32_t>> latencies;   ///< (tick the event was made, latency in ticks)
        /// Its join: connected .. its join data in (ClientStats::joinDataTicks: the baseline ended, no
        /// chunk of its window waiting for the server; 0: not yet).
        uint32_t connectTick = 0, syncTick = 0;
    };

    /// The 99th percentile (nearest rank) of `v`; 0 when empty.
    uint32_t P99(std::vector<uint32_t> v) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, static_cast<size_t>(std::ceil(0.99 * static_cast<double>(v.size()))) - 1)];
    }

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
                             "[--hash-out <file>] [--drill-after N] [--drill-delay S] [--drill-sites K] [--drill-every T] "
                             "[--client-window N] [--walk-across] [--precasts N] [--memory] [--max-join-p99-ticks N]\n");
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
    // A drill that waits for N clients (a listening server beside a session) instead of starting at once.
    const uint32_t drillAfter = static_cast<uint32_t>(Number(args, "--drill-after", 0));
    const double drillDelay = Number(args, "--drill-delay", 0.0);
    const uint32_t drillSites = static_cast<uint32_t>(Number(args, "--drill-sites", 0));
    const uint32_t clientCount = static_cast<uint32_t>(Number(args, "--clients", drillSteps > 0 && !listen ? 2 : 0));
    const uint32_t joinStep = static_cast<uint32_t>(Number(args, "--join-step", drillSteps / 2));
    const uint32_t stepTicks = std::max<uint32_t>(1, static_cast<uint32_t>(Number(args, "--drill-every", 6)));
    // A listening server without a drill or bots runs until --seconds, --exit-when-empty or Ctrl-C.
    const bool openEnded = listen && (drillSteps == 0 || drillAfter > 0) && clientCount == 0 && !args.Has("--ticks");
    const uint32_t ticks = static_cast<uint32_t>(Number(args, "--ticks", drillSteps > 0 ? drillSteps * stepTicks : 120));
    const unsigned threads = static_cast<unsigned>(Number(args, "--threads", 0));
    const bool realtime = enet || args.Has("--realtime");
    const bool stand = args.Has("--stand");
    net::PilotSettings pilotSettings;
    pilotSettings.jumps = !args.Has("--no-jumps");
    pilotSettings.castEveryTicks = static_cast<uint32_t>(Number(args, "--bot-casts", 0));
    pilotSettings.casts = pilotSettings.castEveryTicks > 0;
    pilotSettings.crateEveryTicks = static_cast<uint32_t>(Number(args, "--bot-crates", 0));
    // Stage 7a M-b: the bots' window, the walk across the region, casts before anyone joins.
    const int32_t clientWindow = static_cast<int32_t>(Number(args, "--client-window", 0));
    if (clientWindow < 0 || clientWindow > net::kMaxClientWindowChunks) {
        Log::Error("--client-window expects 0..{} chunks", net::kMaxClientWindowChunks);
        return 2;
    }
    const bool walkAcross = args.Has("--walk-across");
    const uint32_t precasts = static_cast<uint32_t>(Number(args, "--precasts", 0));
    const bool memory = args.Has("--memory");
    // Review of stage 7a: an exit rule on the event latency during joins (networking.md "Testing":
    // p99 under 250 ms = 15 ticks while a client joins); 0 = logged only.
    const uint32_t maxJoinP99 = static_cast<uint32_t>(Number(args, "--max-join-p99-ticks", 0));
    // The game's store: its GPU chunk buffer, 2048 slots kept free for what a session adds.
    constexpr uint32_t kGameStoreSlots = 12288, kGameStoreHeadroom = 2048;

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
        cs.windowChunks = clientWindow;
        if (clientWindow > 0) cs.minFreeStoreSlots = kGameStoreHeadroom;
        bot.joinStep = i == 0 ? 0 : joinStep;
        bot.settings = cs;
        net::PilotSettings ps = pilotSettings;
        if (i == 0 && walkAcross) {
            // The middle z, from the -x edge to the +x edge and back to the middle (metres).
            const sim::RegionDesc& r = server.Region();
            const float chunk = static_cast<float>(voxel::kChunkSize) * r.terrain.voxelSize;
            const float x0 = static_cast<float>(r.playMinChunk.x) * chunk + 4.0f, x1 = static_cast<float>(r.playMaxChunk.x) * chunk - 4.0f;
            const float z = 0.5f * static_cast<float>(r.playMinChunk.y + r.playMaxChunk.y) * chunk;
            ps.waypoints = {{x0, z}, {x1, z}, {0.5f * (x0 + x1), z}};
            ps.casts = false;
        }
        bot.pilot = std::make_unique<net::BotPilot>(server.Region(), 0xb07u + i, ps);
    }
    // --precasts: N server casts along the walker's line (x across the play bounds, the middle z) at
    // the ground, before anyone joins: the walker meets chunks that were modified before it came.
    uint32_t precastsRun = 0;                    // server casts before the drill's (its report skips them)
    if (precasts > 0) {
        const sim::RegionDesc& r = server.Region();
        const voxel::World& w = server.Simulation().World();
        const double chunk = static_cast<double>(voxel::kChunkSize) * static_cast<double>(r.terrain.voxelSize);
        const double x0 = static_cast<double>(r.playMinChunk.x) * chunk + 6.0, x1 = static_cast<double>(r.playMaxChunk.x) * chunk - 6.0;
        const double z = 0.5 * static_cast<double>(r.playMinChunk.y + r.playMaxChunk.y) * chunk;
        const double top = static_cast<double>(r.heightRange.y) - 1.0;
        uint32_t queued = 0;
        for (uint32_t k = 0; k < precasts; ++k) {
            const double x = x0 + (x1 - x0) * (static_cast<double>(k) + 0.5) / static_cast<double>(precasts);
            const double zz = z + (k % 2 == 0 ? -3.0 : 3.0);      // either side of the line: the walker passes between
            const voxel::WorldRayHit hit = w.Raycast(glm::dvec3(x, top, zz), glm::vec3(0.0f, -1.0f, 0.0f), static_cast<float>(top - r.heightRange.x));
            if (!hit.hit) continue;
            queued += server.QueueCast(sim::BlastDesc{hit.position, 1.5f, 6.0f}) ? 1u : 0u;
            // One every 6 ticks, as the drill's (the cast budget is per tick).
            for (uint32_t t = 0; t < 6 && !server.Failed(); ++t) {
                server.Tick();
                clock += dt;
            }
        }
        for (uint32_t t = 0; server.QueuedCasts() > 0 && t < 6000 && !server.Failed(); ++t) {
            server.Tick();
            clock += dt;
        }
        precastsRun = queued;
        Log::Info("Precasts: {} of {} casts along z = {:.1f} m run before any client joined; opSeq {}, {} modified chunks", queued, precasts, z,
                  server.LastOpSeq(), server.ModifiedChunks());
    }

    std::unique_ptr<net::BlastDrill> drill;
    if (drillSteps > 0) {
        drill = std::make_unique<net::BlastDrill>(server.Region(), server.Simulation().WorldMaterialIds(), server.Simulation().World());
        if (drillSites > 0) drill->KeepSites(drillSites);
        Log::Info("Drill: {} steps on {} sites, one every {} ticks{}", drillSteps, drill->Sites(), stepTicks,
                  drillAfter > 0 ? std::format(", once {} client(s) joined (+{:.1f} s)", drillAfter, drillDelay) : std::string());
    }

    bool draining = false;                       // the drain: the players stand (no more casts)
    double passSeconds = dt;                     // time of the current pass (one tick, or the wall clock's)
    // Over UDP a bot that loads its region keeps the rest running (the server, the other bots): the
    // load's callback runs whole passes without it. On the loopback the clock is simulated and the
    // load stays outside it, as before.
    const BotClient* loading = nullptr;
    std::function<void()> nestedPass;
    // Event latency (stage 7a: "event latency during joins"): the server tick every event went out
    // with ([opSeq - 1]); each bot measures the ticks until it applied it.
    std::vector<uint32_t> madeAt;
    auto stepClients = [&](uint32_t step) {
        while (madeAt.size() < server.LastOpSeq()) madeAt.push_back(server.TickNumber());
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
                bot.connectTick = server.TickNumber();
            }
            if (!bot.connected) continue;
            if (bot.client->State() == net::ClientState::Welcomed && !bot.simulation && loading == nullptr) {
                sim::SimulationConfig base;
                // A moving window streams in play: two workers each unless --threads says otherwise.
                base.jobThreads = threads > 0 ? std::max(1u, threads / 2) : clientWindow > 0 ? 2u : 1u;
                base.physics.threads = 1;
                if (session.chunkCapacity != 0) base.chunkCapacity = session.chunkCapacity;   // else the region's (MakeSimulationConfig)
                // --client-window: the game's world (its packing, its store and headroom; plan amendment 6).
                if (clientWindow > 0) {
                    base.world.packHidden = session.packHidden;
                    base.chunkCapacity = kGameStoreSlots;
                }
                bot.simulation = std::make_unique<sim::Simulation>(bot.client->MakeSimulationConfig(base, clientWindow > 0 ? kGameStoreSlots : 0));
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
                if (bot.client->Predicting()) {
                    bot.minX = std::min(bot.minX, bot.client->Player().position.x);
                    bot.maxX = std::max(bot.maxX, bot.client->Player().position.x);
                }
                if (bot.simulation)
                    for (const sim::BlastResult& blast : bot.client->TakePresentations()) sim::PresentBlast(*bot.simulation, blast);
            }
            if (bot.simulation && bot.client->State() == net::ClientState::Playing) {
                if (!bot.counting) {
                    bot.counting = true;
                    bot.seen = bot.client->JoinOpSeq();
                }
                const uint32_t tick = server.TickNumber();
                for (; bot.seen < bot.client->AppliedOpSeq() && bot.seen < madeAt.size(); ++bot.seen)
                    bot.latencies.emplace_back(madeAt[bot.seen], tick - madeAt[bot.seen]);
                if (bot.syncTick == 0 && bot.client->Stats().joinDataTicks != 0) bot.syncTick = std::max(tick, bot.connectTick + 1);
            }
            bot.link->Flush();
        }
    };

    uint32_t step = 0;
    uint32_t drilledTick = 0xFFFFFFFFu;          // a pass that runs no tick must not queue the step again
    // --drill-after: the tick the drill starts on, once enough clients joined (0xFFFFFFFF: not yet).
    uint32_t drillStart = drillAfter > 0 ? 0xFFFFFFFFu : 0u;
    auto drillStep = [&](uint32_t t) {
        if (drill && drillStart == 0xFFFFFFFFu && server.ClientCount() >= drillAfter) {
            drillStart = t + static_cast<uint32_t>(std::lround(drillDelay / dt));
            Log::Info("Drill: {} client(s) joined at tick {}: the drill starts at tick {}", server.ClientCount(), t, drillStart);
        }
        if (drill && t >= drillStart && (t - drillStart) % stepTicks == 0 && step < drillSteps && t != drilledTick) {
            drilledTick = t;
            const net::DrillStep s = drill->Step(step);
            if (!net::RunDrillStep(server, s)) Log::Warn("Drill: step {} cast outside the play bounds", step);
            ++step;
        }
    };
    // The replicated objects at this opSeq, as a game client logs its copies (tools/session-check.sh).
    auto logEntities = [&server] {
        const std::vector<net::EntityInfo> entities = server.Entities();
        size_t resting = 0;
        for (const net::EntityInfo& e : entities) resting += e.resting ? 1u : 0u;
        Log::Info("Session: entities at opSeq {}: {} object(s), {} resting, id hash {:016x}", server.LastOpSeq(), entities.size(), resting,
                  net::EntityListHash(entities));
    };
    // Clients that left a listening server: logged with the server's opSeq and hash at that moment,
    // before its player's crates are despawned (the opSeq the client drained to).
    server.SetOnClientLeft([&server, &logEntities](uint8_t slot) {
        const voxel::WorldHashResult h = server.Hash();
        // Stage 7a: and the hash over that client's sync box (its window inside the server's), the
        // box its own hash covers (the region's window for a client whose window is the region's).
        const net::SyncBox box = server.ClientSyncBox(slot);
        const voxel::WorldHashResult wh = server.Hash(box);
        const glm::ivec3 max = box.Max();
        Log::Info("Session: the client of slot {} left; server at opSeq {}, tick {}, world hash {:016x} ({} chunks); window hash {:016x} over "
                  "[{},{},{}..{},{},{})",
                  slot, server.LastOpSeq(), server.TickNumber(), h.hash, h.solidChunks, wh.hash, box.min.x, box.min.y, box.min.z, max.x, max.y,
                  max.z);
        logEntities();
    });
    size_t lastClients = 0, mostClients = 0;
    uint32_t lastReported = 0;
    auto watchClients = [&] {
        const size_t n = server.ClientCount();
        if (n < lastClients) {
            const voxel::WorldHashResult h = server.Hash();
            Log::Info("Session: {} client(s) left; server at opSeq {}, tick {}, world hash {:016x} ({} chunks)", lastClients - n,
                      server.LastOpSeq(), server.TickNumber(), h.hash, h.solidChunks);
            logEntities();
        }
        mostClients = std::max(mostClients, n);
        lastClients = n;
    };
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    // --memory: the process's resident memory, the server world's chunks and each bot's store.
    uint32_t lastMemory = 0;
    auto logMemory = [&] {
        const voxel::WorldStats ws = server.Simulation().World().Stats();
        std::string line = std::format("Memory: tick {}, RSS {:.0f} MiB (every simulation of this process); server world {} dense / {} packed "
                                       "chunks, {:.1f} MiB RLE, store {} / {}",
                                       server.TickNumber(), ResidentMiB(), ws.denseChunks, ws.packedChunks,
                                       static_cast<double>(ws.packedBytes) / (1024.0 * 1024.0), server.Simulation().Chunks().UsedCount(),
                                       server.Simulation().Chunks().Capacity());
        for (size_t i = 0; i < bots.size(); ++i)
            if (bots[i].simulation)
                line += std::format("; bot {} store {} / {} (max {})", i, bots[i].simulation->Chunks().UsedCount(),
                                    bots[i].simulation->Chunks().Capacity(), bots[i].client->Stats().maxStoreUsed);
        Log::Info("{}", line);
    };
    double lastWall = 0.0;
    double maxPassSeconds = 0.0;                 // wall clock of the server's work in one pass (UDP)
    uint32_t slowPasses = 0;                     // ... passes longer than a tick
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
            // How long the server's own work took (its ticks and the drill's edits): a pass over a
            // tick's length is a server hitch, which clients see as a growing input margin.
            const double busy = wallNow() - t;
            maxPassSeconds = std::max(maxPassSeconds, busy);
            slowPasses += busy > dt ? 1u : 0u;
            if (busy > 3.0 * dt)
                Log::Info("Server: a pass took {:.1f} ms at tick {} ({}){}", busy * 1000.0, server.TickNumber(),
                          server.TickNumber() > before ? std::format("{} tick(s)", server.TickNumber() - before) : std::string("no tick"),
                          drilledTick == before || drilledTick == before + 1 ? ", with a drill step" : "");
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
                // Stage 7a: no join log; the modified map is what a joiner may be sent.
                Log::Info("Server: tick {}, {} client(s), opSeq {}, {} modified chunks, backlog {:.1f} KiB, bulk {:.1f} KiB sent, {} casts "
                          "run ({} from players), {} snapshots sent",
                          server.TickNumber(), server.ClientCount(), server.LastOpSeq(), server.ModifiedChunks(),
                          static_cast<double>(server.BacklogBytes()) / 1024.0, static_cast<double>(st.bulkBytes) / 1024.0, st.castsRun,
                          st.castsFromInputs, st.snapshots);
            }
        }
        if (memory && server.TickNumber() / 600 != lastMemory) {
            lastMemory = server.TickNumber() / 600;
            logMemory();
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
        // --walk-across: the run lasts until the walker is done too (at most 20 simulated minutes).
        auto walking = [&] { return walkAcross && !bots.empty() && !bots[0].pilot->Done() && server.TickNumber() < 72000; };
        while ((server.TickNumber() < ticks || walking()) && !g_Stop) {
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
        // Stage 7a: and every bot is synced (its chunks current, no request out, its baseline in).
        for (const BotClient& bot : bots)
            if (!bot.simulation || bot.client->AppliedOpSeq() != server.LastOpSeq() || !bot.client->Synced()) return false;
        return true;
    };
    const uint32_t drainStart = server.TickNumber();
    const double drainWall = wallNow();
    // A moving client window (--client-window) may still be generating its far (cave) chunks when the
    // walk ends (a bot's workers, not its ticks, decide how long): its drain goes on while a bot's
    // world has chunks to load, up to two minutes of wall clock.
    bool loadingLeft = clientWindow > 0;
    auto drainMore = [&] {
        if (server.TickNumber() - drainStart < 600) return true;
        if (!loadingLeft || wallNow() - drainWall > 120.0) return false;
        if ((server.TickNumber() - drainStart) % 60 == 0) {
            loadingLeft = false;
            for (const BotClient& bot : bots)
                if (bot.simulation && bot.simulation->World().Stats().pendingChunks > 0) loadingLeft = true;
        }
        return true;
    };
    // Ctrl-C skips it (and fails the run: the hashes were not compared after a drain).
    while (!openEnded && !g_Stop && !server.Failed() && !settled() && drainMore()) {
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
        if (precastsRun > 0 && i < precastsRun) continue;   // --precasts, before the drill
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
    Log::Info("Server: {} clients joined, {} left, {} refused, {} peers timed out, {} too slow, {} broken sync streams",
              st.clientsJoined, st.clientsDisconnected, st.clientsRefused, st.peersTimedOut, st.peersTooSlow, st.malformedRequests);
    Log::Info("Sync: {} modified chunks; {} window updates, {} acks marking {} chunks; {} ChunkData / {:.1f} KiB ({} dropped at dequeue), "
              "{} baseline entities / {:.1f} KiB ({} gone), {} chunk and {} model requests ({} ignored); bulk {:.1f} KiB, held {} times, "
              "window {:.1f}..{:.1f} KiB, {} over 16 KiB, {} past the window alone; {} near-set markers, {} repeated model requests; bulk "
              "work {:.1f} ms in all, at most {:.2f} ms in a tick",
              server.ModifiedChunks(), st.windowUpdates, st.windowAcks, st.ackedModified, st.chunkData,
              static_cast<double>(st.chunkDataBytes) / 1024.0, st.chunksDropped, st.baselineEntities, static_cast<double>(st.baselineBytes) / 1024.0,
              st.baselineDropped, st.chunkRequests, st.modelRequests, st.requestsIgnored, static_cast<double>(st.bulkBytes) / 1024.0, st.bulkHeld,
              static_cast<double>(st.minBulkWindow) / 1024.0, static_cast<double>(st.maxBulkWindow) / 1024.0, st.bulkOversize, st.bulkAlone,
              st.baselineNear, st.modelRequestRepeats, st.encodeSeconds * 1000.0, st.maxEncodeSecondsPerTick * 1000.0);
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
        Log::Info("Client {} objects: {} copies made ({} failed, {} lost), {} despawned ({} kept as rubble), {} rests ({} stale), {} "
                  "wakes, {} models, {} fragments; {} body samples taken, {} ignored, largest sample gap {} ticks; {} ticks a copy carried "
                  "the player; pilot crates {}",
                  i, cs.copiesMade, cs.copiesFailed, cs.copiesLost, cs.despawns, cs.toRubble, cs.rests, cs.staleRests, cs.wakes, cs.models,
                  cs.fragments, cs.bodySamples, cs.bodySamplesIgnored, bots[i].client->Bodies().MaxGap(), cs.heldUnderPlayer,
                  bots[i].pilot->Crates());
    }
    {
        const sim::ReplicationStats& rs = server.Simulation().Replication().stats;
        Log::Info("Objects: {} server bodies from terrain pieces, {} object spawns, {} fragments as client rubble, {} models ({} let go "
                  "over the entity byte budget), {} rests, {} wakes, despawns {} destroyed / {} to rubble / {} broken; pieces: {} replicated, "
                  "{} over the tick budget, {} over the awake cap, {} over the byte budget, {} evicted ({} resting, {} awake), {} body "
                  "failures, at most {} live / {} awake; crates {} ({} refused); {} casts hit a body; entity events {:.1f} KiB; snapshots up "
                  "to {} B with {} bodies, {} bodies sent, {} left out",
                  st.detachBodies, st.objectSpawns, st.fragmentSpawns, st.objectModels, st.modelsToRubble, st.rests, st.wakes, st.despawns[1],
                  st.despawns[2], st.despawns[3], rs.pieces, rs.overTickBudget, rs.overAwakeCap, rs.overByteBudget,
                  rs.evictedResting + rs.evictedAwake, rs.evictedResting, rs.evictedAwake, rs.bodyFailures, rs.maxLive, rs.maxAwake, st.crates,
                  st.cratesRefused, st.castsAtBodies, static_cast<double>(st.entityEventBytes) / 1024.0, st.maxSnapshotBytes,
                  st.maxSnapshotBodies, st.bodySamples, st.bodiesLeftOut);
    }
    if (memory) logMemory();
    const voxel::WorldStats ws = server.Simulation().World().Stats();
    Log::Info("Server: world {} dense / {} packed chunks, {:.1f} MiB RLE, store {} / {} slots, search errors {}, incomplete ops {}, "
              "RSS {:.0f} MiB (all simulations of this process)",
              ws.denseChunks, ws.packedChunks, static_cast<double>(ws.packedBytes) / (1024.0 * 1024.0),
              server.Simulation().Chunks().UsedCount(), server.Simulation().Chunks().Capacity(), st.searchErrors, st.incompleteOps,
              ResidentMiB());

    const voxel::WorldHashResult sh = server.Hash();
    bool ok = !server.Failed() && sh.Complete() && !interrupted;
    // The entity check (stage 6): the same NetIds and models on every side, the same rest state, rest
    // poses within 5 cm and 1 mrad (review of stage 6).
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
    logEntities();
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
        // Stage 7a: compared over the bot's sync box (its window inside the server's) at the same opSeq,
        // once it is synced; for a fixed-window bot that box is the region's window.
        const voxel::WorldHashResult sb = server.Hash(bot.client->SyncWindow());
        const bool equal = bot.simulation && bot.client->Synced() && ch.Complete() && sb.Complete() && ch.hash == sb.hash &&
                           bot.client->AppliedOpSeq() == server.LastOpSeq();
        ok = ok && equal && cs.skippedChunks == 0 && cs.probeMismatches == 0;
        Log::Info("Client {}: {} at opSeq {} / {}, joined at opSeq {}, {} events / {:.1f} KiB, {} duplicates, {} probe mismatches, {} skipped / "
                  "{} unsynced chunks, hash {:016x} {}",
                  i, bot.simulation ? "attached" : "not attached", bot.client->AppliedOpSeq(), server.LastOpSeq(), bot.client->JoinOpSeq(),
                  cs.eventsApplied, static_cast<double>(cs.eventBytes) / 1024.0, cs.duplicateEvents, cs.probeMismatches, cs.skippedChunks,
                  cs.unsyncedChunks, ch.hash, equal ? "equal" : "DIFFERENT");
        Log::Info("Join: bot {} started after {:.2f} s of server ticks ({:.2f} s wall; chunks within 32 m current, entities within 32 m of the spawn in (BaselineNear)), join "
                  "data in after {:.2f} s ({:.2f} s wall; no chunk waiting for the server), synced after {:.2f} s ({:.2f} s wall; every "
                  "chunk current), {:.1f} KiB bulk "
                  "received ({} ChunkData, {} baseline entities, {} discarded), {} chunk / {} model requests, {} ops skipped for chunks "
                  "without a base, {} replayed; CPU: sync {:.3f} ms per Poll (max {:.2f}), bulk installs {:.1f} ms in all (max {:.2f} ms, {} "
                  "in one Poll)",
                  i, static_cast<double>(cs.joinStartTicks) * tickSeconds, cs.joinStartSeconds, static_cast<double>(cs.joinDataTicks) * tickSeconds,
                  cs.joinDataSeconds, static_cast<double>(cs.joinSyncTicks) * tickSeconds, cs.joinSyncSeconds,
                  static_cast<double>(cs.bulkBytes) / 1024.0, cs.chunkData, cs.baselineEntities, cs.baselineDiscarded, cs.chunkRequests,
                  cs.modelRequests, bot.client->Sync().Stats().chunkSkips, bot.client->Sync().Stats().replays,
                  cs.polls > 0 ? cs.syncSeconds * 1000.0 / static_cast<double>(cs.polls) : 0.0, cs.maxSyncSeconds * 1000.0,
                  cs.bulkSeconds * 1000.0, cs.maxBulkSeconds * 1000.0, cs.maxBulkInPoll);
        {
            // Event latency (server ticks from an event being made to this bot applying it), for the
            // events made while some bot was joining (connected, its join data not in yet) and for all.
            const auto duringJoin = [&bots](uint32_t made) {
                for (const BotClient& b : bots)
                    if (b.connected && made >= b.connectTick && (b.syncTick == 0 || made < b.syncTick)) return true;
                return false;
            };
            std::vector<uint32_t> joinLat, allLat;
            for (const auto& [made, latency] : bot.latencies) {
                allLat.push_back(latency);
                if (duringJoin(made)) joinLat.push_back(latency);
            }
            const auto maxOf = [](const std::vector<uint32_t>& v) { return v.empty() ? 0u : *std::max_element(v.begin(), v.end()); };
            std::string joins;
            for (size_t j = 0; j < bots.size(); ++j)
                if (bots[j].connected)
                    joins += std::format("{}bot {} {}..{}", joins.empty() ? "" : ", ", j, bots[j].connectTick,
                                         bots[j].syncTick != 0 ? std::to_string(bots[j].syncTick) : std::string("not synced"));
            const bool latencyOk = maxJoinP99 == 0 || P99(joinLat) < maxJoinP99;     // under N ticks, as the unit test (recheck of 7a, 6)
            ok = ok && latencyOk;
            Log::Info("Latency: bot {}: events made during joins ({}): p99 {} ticks ({:.0f} ms), max {} over {} events; all events: p99 {} "
                      "ticks ({:.0f} ms), max {} over {}{}",
                      i, joins, P99(joinLat), P99(joinLat) * tickSeconds * 1000.0, maxOf(joinLat), joinLat.size(), P99(allLat),
                      P99(allLat) * tickSeconds * 1000.0, maxOf(allLat), allLat.size(),
                      latencyOk ? std::string() : std::format(" - NOT UNDER --max-join-p99-ticks {}", maxJoinP99));
        }
        if (bot.simulation) {
            // The client's window and store (stage 7a M-b): a bot with the game's window must never
            // fill its store, and must keep the game's headroom free.
            const voxel::WorldStats bws = bot.simulation->World().Stats();
            const uint32_t capacity = bot.simulation->Chunks().Capacity();
            const bool storeOk = bws.storeFullLoads == 0 && bws.storeFullDecodes == 0 && cs.unsyncedChunks == 0 &&
                                 (clientWindow == 0 || cs.maxStoreUsed + kGameStoreHeadroom <= capacity);
            ok = ok && storeOk;
            const net::SyncBox w = bot.client->Window(), sbx = bot.client->SyncWindow();
            Log::Info("Client {} window: {} x {} x {} chunks at ({}, {}, {}){}, sync box {} x {} x {}; {} window updates ({} resets); store "
                      "{} / {} slots, at most {} used, store full {} / {}{}; {} dense / {} packed chunks; {} ChunkData installed, {} stale",
                      i, w.size.x, w.size.y, w.size.z, w.min.x, w.min.y, w.min.z, bot.client->WindowMoves() ? " (follows the player)" : "",
                      sbx.size.x, sbx.size.y, sbx.size.z, cs.windowUpdates, cs.windowResets, bot.simulation->Chunks().UsedCount(), capacity,
                      cs.maxStoreUsed, bws.storeFullLoads, bws.storeFullDecodes, storeOk ? "" : " - OVER THE GATE", bws.denseChunks,
                      bws.packedChunks, bot.client->Sync().Stats().chunksInstalled, bot.client->Sync().Stats().staleChunkData);
            if (!bot.client->Synced()) {
                const net::ChunkSyncCounts n = bot.client->Sync().Counts();
                Log::Info("Client {} is not synced: {} chunks current, {} without a base, {} awaiting data, {} based but not loaded ({} "
                          "pending in the world), {} requested, {} unsynced",
                          i, n.current, n.noBase, n.awaiting, n.basedNotReady, bws.pendingChunks, n.requested, n.unsynced);
            }
            if (i == 0 && walkAcross)
                Log::Info("Client 0 walk: {} of {} waypoints reached, {} given up, x from {:.1f} to {:.1f} m, feet at ({:.1f}, {:.1f}, {:.1f}); {} ticks held over chunks "
                          "without a base, {} snapshots taken as they were, {} resumes",
                          bot.pilot->WaypointsReached(), 3, bot.pilot->WaypointsGivenUp(), bot.minX, bot.maxX, bot.client->Player().position.x,
                          bot.client->Player().position.y, bot.client->Player().position.z, bot.client->Prediction().syncHeldTicks,
                          bot.client->Prediction().syncTaken, bot.client->Prediction().syncResumes);
        }
        // The server's hash over the bot's sync box beside it (the region's hash for a fixed window).
        hashes += std::format(", client {} {:016x} (server {:016x} over its sync box)", i, ch.hash, sb.hash);
        if (bot.udp)
            Log::Info("Client {} UDP: {} of {} datagrams dropped on receive, RTT {:.1f} ms, {} messages sent / {} received", i,
                      bot.udp->Stats().datagramsDropped, bot.udp->Stats().datagramsReceived, bot.udp->RoundTripSeconds(1) * 1000.0,
                      bot.udp->Stats().messagesSent, bot.udp->Stats().messagesReceived);
    }
    if (serverUdp)
        Log::Info("Server: longest pass {:.1f} ms of server work (ticks and drill edits), {} pass(es) longer than a tick", maxPassSeconds * 1000.0,
                  slowPasses);
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
