// vortex_server: the session server of docs/design/networking.md (stages 3-6).
//
//   vortex_server --session <file> [--listen [PORT]] [--bind ADDR] [--token HEX | --new-token]
//                 [--seconds S] [--exit-when-empty] [--parent-pid PID]
//                 [--ticks N] [--drill N] [--clients N] [--enet] [--join-step K]
//                 [--loss P] [--delay-ms D] [--jitter-ms J] [--outage-ms A,B] [--threads T]
//                 [--stand] [--no-jumps] [--bot-casts N] [--bot-crates N] [--realtime] [--hash-out <file>]
//                 [--drill-after N] [--drill-delay S] [--drill-sites K] [--drill-every T]
//                 [--client-window N] [--walk-across] [--precasts N] [--memory] [--max-join-p99-ticks N]
//                 [--bot-broken-chunk N[:X,Y,Z]] [--bot-broken-generator N] [--timeline FILE] [--broken-chunk-candidate]
//                 [--terrain-bulk-ceiling KIB] [--bulk-window-floor KIB] [--pacer-delivery-interval S]
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
// Stage 7b: every bot's HashChecks and EntityChecks are counted (`Checks: bot N:` lines, the server's
// `Checks: server:` line), and a bot that reports a hash or entity mismatch or a check out of its stream
// position fails the run (no fault was injected: the checks' false positives). --bot-broken-chunk N
// gives bot N a generator that makes one pristine chunk differently (by default the server-Dense
// pristine chunk nearest the spawn that is not a probe; N:X,Y,Z names one, refused unless it is
// pristine, server-Dense, not a probe and within 12 m of the spawn); that bot stands, and the run fails
// unless a HashCheck found the chunk and its repair was installed.
// --bot-broken-generator N (stage 7b M-b) gives bot N another build's terrain everywhere (the region's
// generator with the terrain seed + 1, or a flat region's ground 1.5 m higher): its probe chunks
// differ at the Welcome, so it plays in server terrain mode (the server sends its whole window); the
// run fails unless it did and still ends with the server's world and objects. Every bot in terrain
// mode logs a `Terrain: bot N:` line (why and since when, the acks, the data, held ticks) with the
// server's bytes for its join and per window step.
// Stage 8: the bots are net::Bot (shared with vortex_bot, which runs one bot per process); a broken-chunk bot
// stands until its chunk is repaired, then plays, and every bot's verdict is net::JudgeBot's. --timeline FILE
// writes `ops <last opSeq> <steady-clock ns> <clients joining>` after every pass that made events or changed the
// joining count (vortex_bot --timeline reads it: event latency on one host). --broken-chunk-candidate logs `Broken
// chunk candidate: X,Y,Z` (a pristine server-Dense chunk 3 m under the ground near the spawn) for vortex_bot
// --broken-chunk. The server's hashes are cached per chunk revision and warmed before anyone joins (a leave costs
// lookups); join, first-window and leave ticks are timed apart (`Ticks: tick N (...) took X ms`).
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

#if defined(__linux__)
#include <sys/prctl.h>
#include <unistd.h>
#endif

import vortex.core;
import vortex.voxel;
import vortex.scene;
import vortex.physics;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
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

    /// An in-process bot (stage 8: the bot itself is net::Bot, shared with vortex_bot): its transport
    /// chain and when it joined.
    struct BotClient {
        std::unique_ptr<net::LoopbackTransport> loop;
        std::unique_ptr<net::EnetTransport> udp;   ///< --enet: the bot's own UDP socket
        std::unique_ptr<net::ImpairedTransport> link;
        std::unique_ptr<net::Bot> bot;
        net::BotSettings settings;
        bool connected = false;
        uint32_t joinStep = 0;
        /// Its join: connected .. its join data in (ClientStats::joinDataTicks: the baseline ended, no
        /// chunk of its window waiting for the server; 0: not yet).
        uint32_t connectTick = 0, syncTick = 0;
    };

    /// The 99th percentile (nearest rank) of `v`; 0 when empty.
    uint32_t P99(const std::vector<uint32_t>& v) {
        std::vector<double> d(v.begin(), v.end());
        return static_cast<uint32_t>(net::PercentilesOf(std::move(d)).p99);
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
                             "[--client-window N] [--walk-across] [--precasts N] [--memory] [--max-join-p99-ticks N] "
                             "[--bot-broken-chunk N[:X,Y,Z]] [--bot-broken-generator N] [--timeline FILE] [--broken-chunk-candidate] [--terrain-bulk-ceiling KIB] [--bulk-window-floor KIB] [--pacer-delivery-interval S]\n");
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
    // The drill report reads every drill cast (review of stage 8: players' casts and the precasts share the
    // record; a limit of the drill's steps alone dropped the oldest drill casts in a 10-minute soak and the
    // report counted the wrong steps). Records are about 100 bytes.
    if (drillSteps > 0) settings.castRecordLimit = std::max<uint32_t>(settings.castRecordLimit, drillSteps + precasts + 65536u);
    // --terrain-bulk-ceiling KIB (stage 8, O2): the bulk window's ceiling for a peer in server terrain mode.
    if (const std::optional<std::string> kib = args.Get("--terrain-bulk-ceiling"))
        settings.terrainBulkCeilingBytes = static_cast<size_t>(std::max(0.0, std::atof(kib->c_str())) * 1024.0);
    // --bulk-window-floor KIB (stage 8, a measurement knob): the bulk window never goes under this (the ceiling is
    // raised to it): with the floor at the ceiling the window is fixed, which tells the pacer's share of a slow
    // join from the link's (networking.md "Stage 8 as built", O2 / O5).
    // --pacer-delivery-interval S (stage 8, a measurement knob): BulkPacerSettings::deliveryInterval; 0 = the 7a
    // pacer (its window sized by the smoothed drain rate alone).
    if (const std::optional<std::string> sec = args.Get("--pacer-delivery-interval")) {
        const double v = std::atof(sec->c_str());
        settings.bulkPacer.deliveryInterval = v > 0.0 ? v : 1.0e12;
    }
    if (const std::optional<std::string> kib = args.Get("--bulk-window-floor")) {
        settings.bulkPacer.floorBytes = std::max<size_t>(1200, static_cast<size_t>(std::max(0.0, std::atof(kib->c_str())) * 1024.0));
        settings.bulkPacer.ceilingBytes = std::max(settings.bulkPacer.ceilingBytes, settings.bulkPacer.floorBytes);
    }
    net::Server server(settings, serverLink);
    if (!server.Start()) return 1;
    const double dt = static_cast<double>(server.Simulation().FixedTimeStep());

    std::vector<BotClient> bots(clientCount);
    for (uint32_t i = 0; i < clientCount; ++i) {
        BotClient& bot = bots[i];
        bot.loop = std::make_unique<net::LoopbackTransport>();
        net::BotSettings& bs = bot.settings;
        bs.id = i;
        bs.client.token = session.token;
        bs.client.presentation = i == 0;   // the first client also makes particles and rubble, as a game would
        bs.client.windowChunks = clientWindow;
        bs.pilot = pilotSettings;
        bs.walkAcross = i == 0 && walkAcross;
        bs.stand = stand;
        // A moving window streams in play: two workers each unless --threads says otherwise.
        bs.jobThreads = threads > 0 ? std::max(1u, threads / 2) : clientWindow > 0 ? 2u : 1u;
        bs.chunkCapacity = session.chunkCapacity;
        bs.packHidden = session.packHidden;
        bot.joinStep = i == 0 ? 0 : joinStep;
    }
    // --bot-broken-chunk N[:X,Y,Z] (stage 7b): one bot's generator makes one pristine chunk differently.
    glm::ivec3 brokenChunk{0};
    if (const std::optional<std::string> spec = args.Get("--bot-broken-chunk")) {
        int n = -1, x = 0, y = 0, z = 0;
        const int fields = std::sscanf(spec->c_str(), "%d:%d,%d,%d", &n, &x, &y, &z);
        if ((fields != 1 && fields != 4) || n < 0 || static_cast<uint32_t>(n) >= clientCount) {
            Log::Error("--bot-broken-chunk expects N or N:X,Y,Z with N a bot (0..{})", clientCount == 0 ? 0 : clientCount - 1);
            return 2;
        }
        if (fields == 4) {
            brokenChunk = glm::ivec3(x, y, z);
            std::string why;
            if (!net::UsableBrokenChunk(server, brokenChunk, &why)) {
                Log::Error("--bot-broken-chunk: chunk ({}, {}, {}) is {}", x, y, z, why);
                return 2;
            }
        } else {
            std::string why;
            if (!net::BrokenChunkCandidate(server, brokenChunk, &why)) {
                Log::Error("--bot-broken-chunk: {}", why);
                return 2;
            }
        }
        BotClient& bot = bots[static_cast<size_t>(n)];
        bot.settings.hasBrokenChunk = true;
        bot.settings.brokenChunk = brokenChunk;
        const uint8_t stone = server.Simulation().WorldMaterialIds().stone;
        bot.settings.client.wrapGenerator = [brokenChunk, stone](std::shared_ptr<voxel::IWorldGenerator> g) {
            return net::BreakChunks(std::move(g), {brokenChunk}, stone);
        };
        const double chunkMetres = static_cast<double>(voxel::kChunkSize) * static_cast<double>(server.Simulation().World().VoxelSize());
        Log::Info("Broken chunk: bot {}'s generator makes chunk ({}, {}, {}) differently ({:.1f} m from the spawn); it stands until it is "
                  "repaired", n, brokenChunk.x, brokenChunk.y, brokenChunk.z,
                  glm::distance((glm::dvec3(brokenChunk) + glm::dvec3(0.5)) * chunkMetres, glm::dvec3(server.Template().spawn)));
    }

    // --bot-broken-generator N (stage 7b M-b): one bot's build makes other terrain everywhere.
    if (const std::optional<std::string> spec = args.Get("--bot-broken-generator")) {
        int n = -1;
        if (std::sscanf(spec->c_str(), "%d", &n) != 1 || n < 0 || static_cast<uint32_t>(n) >= clientCount) {
            Log::Error("--bot-broken-generator expects N, a bot (0..{})", clientCount == 0 ? 0 : clientCount - 1);
            return 2;
        }
        BotClient& bot = bots[static_cast<size_t>(n)];
        if (bot.settings.hasBrokenChunk) {
            Log::Error("--bot-broken-generator: bot {} already has --bot-broken-chunk", n);
            return 2;
        }
        bot.settings.brokenGenerator = true;
        const sim::RegionDesc region = server.Region();
        const voxel::WorldMaterials ids = server.Simulation().WorldMaterialIds();
        bot.settings.client.wrapGenerator = [region, ids](std::shared_ptr<voxel::IWorldGenerator>) {
            sim::RegionDesc other = region;
            other.terrain.seed += 1;
            other.flatGroundVoxels += 12;
            return sim::MakeRegionGenerator(other, ids);
        };
        Log::Info("Broken generator: bot {} makes other terrain everywhere ({}); it must play in server terrain mode", n,
                  region.generator == sim::RegionGenerator::Flat ? "its flat ground 12 voxels higher" : "terrain seed + 1");
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
    std::function<void()> nestedPass;
    auto anyLoading = [&bots] {
        for (const BotClient& b : bots)
            if (b.bot && b.bot->Loading()) return true;
        return false;
    };
    // Event latency (stage 7a: "event latency during joins"): the server tick every event went out
    // with ([opSeq - 1]); each bot measures the ticks until it applied it.
    std::vector<uint32_t> madeAt;
    auto stepClients = [&](uint32_t step) {
        while (madeAt.size() < server.LastOpSeq()) madeAt.push_back(server.TickNumber());
        const bool loading = anyLoading();
        for (BotClient& bot : bots) {
            if (bot.bot && bot.bot->Loading()) continue;
            if (loading && !bot.connected) continue;   // no new joins inside a load
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
                bot.bot = std::make_unique<net::Bot>(bot.settings, *bot.link);
                bot.connected = true;
                bot.connectTick = server.TickNumber();
            }
            if (!bot.connected) continue;
            net::Client& client = bot.bot->GetClient();
            // One region load at a time (over UDP the others keep running inside it).
            if (loading && client.State() == net::ClientState::Welcomed && bot.bot->GetSimulation() == nullptr) continue;
            const bool hadSimulation = bot.bot->GetSimulation() != nullptr;
            bot.bot->Pass(passSeconds, !draining, [&nestedPass, enet] {
                if (enet) nestedPass();
            });
            // The rest of the load (after its last callback: the final checks) for everyone else too;
            // the passes inside the load have given them the time before.
            if (!hadSimulation && bot.bot->GetSimulation() != nullptr && enet) {
                nestedPass();
                passSeconds = 0.0;
            }
            const uint32_t tick = server.TickNumber();
            bot.bot->RecordApplied(static_cast<double>(tick));
            if (bot.bot->GetSimulation() && client.State() == net::ClientState::Playing && bot.syncTick == 0 &&
                client.Stats().joinDataTicks != 0)
                bot.syncTick = std::max(tick, bot.connectTick + 1);
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
        Log::Info("{}", net::ServerEntitiesLine(server));
        // Review of stage 8: the objects that are not resting, positions to 1 mm (scripts compare them with
        // 5 cm of tolerance: the rest hash covers resting objects only).
        Log::Info("{}", net::ServerAwakeLine(server));
        // Stage 8: the replicated bodies that are not resting, and why (a frozen body has no terrain
        // collider under it; one that is awake but frozen never sends an EntityRest).
        std::string awake;
        uint32_t count = 0;
        physics::PhysicsWorld& physics = server.Simulation().Physics();
        for (const auto [entity, id, body] : server.Simulation().Scene().Registry().view<const scene::NetId, const scene::RigidBody>().each()) {
            if (!sim::IsAwake(physics, body.body)) continue;
            const physics::BodyState st = physics.GetState(body.body);
            if (++count <= 4)
                awake += std::format("{}{} at ({:.1f}, {:.1f}, {:.1f}){}{}", count > 1 ? ", " : "", id.value, st.position.x, st.position.y,
                                     st.position.z, st.frozen ? " frozen" : "", st.active ? " active" : "");
        }
        if (count > 0) Log::Info("Session: {} replicated object(s) not resting: {}{}", count, awake, count > 4 ? ", ..." : "");
    };
    // Clients that left a listening server: logged with the server's opSeq and hash at that moment,
    // before its player's crates are despawned (the opSeq the client drained to).
    server.SetOnClientLeft([&server, &logEntities](uint8_t slot) {
        // Its hashes at this opSeq (the per-chunk cache: stage 8) and its join point (recheck of stage 8:
        // a slot is reused, so the scripts match a client to its leave line by slot and join point).
        Log::Info("{}", net::ServerLeaveLine(server, slot));
        logEntities();
    });
    size_t lastClients = 0, mostClients = 0;
    uint32_t lastReported = 0;
    auto watchClients = [&] {
        const size_t n = server.ClientCount();
        if (n < lastClients) {
            // The hashes are in the per-slot lines above (stage 8: no third hash of the window per leave).
            Log::Info("Session: {} client(s) left; server at opSeq {}, tick {}", lastClients - n, server.LastOpSeq(), server.TickNumber());
        }
        mostClients = std::max(mostClients, n);
        lastClients = n;
    };
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    // --parent-pid (review of stage 8): a script killed with SIGKILL runs no trap; its server must not keep
    // the port and the cores (Linux: SIGTERM when the parent is gone, which ends the run as Ctrl-C does).
    if (const std::optional<std::string> parent = args.Get("--parent-pid")) {
#if defined(__linux__)
        const pid_t pid = static_cast<pid_t>(std::atol(parent->c_str()));
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (pid <= 0 || getppid() != pid) {
            Log::Error("--parent-pid {}: that is not this process's parent (gone already?)", *parent);
            return 2;
        }
#else
        Log::Warn("--parent-pid is Linux only: ignored");
#endif
    }

    // --memory: the process's resident memory, the server world's chunks and each bot's store.
    uint32_t lastMemory = 0;
    auto logMemory = [&] {
        const voxel::WorldStats ws = server.Simulation().World().Stats();
        std::string line = std::format("Memory: tick {}, RSS {:.0f} MiB (every simulation of this process); server world {} dense / {} packed "
                                       "chunks, {:.1f} MiB RLE, store {} / {}",
                                       server.TickNumber(), net::ResidentMiB(), ws.denseChunks, ws.packedChunks,
                                       static_cast<double>(ws.packedBytes) / (1024.0 * 1024.0), server.Simulation().Chunks().UsedCount(),
                                       server.Simulation().Chunks().Capacity());
        for (size_t i = 0; i < bots.size(); ++i)
            if (bots[i].bot && bots[i].bot->GetSimulation())
                line += std::format("; bot {} store {} / {} (max {})", i, bots[i].bot->GetSimulation()->Chunks().UsedCount(),
                                    bots[i].bot->GetSimulation()->Chunks().Capacity(), bots[i].bot->GetClient().Stats().maxStoreUsed);
        Log::Info("{}", line);
    };
    // --timeline FILE (stage 8): the steady-clock time of each pass that made events or changed the
    // number of joining clients, for vortex_bot's event latency on the same host (one shared clock).
    std::ofstream timeline;
    if (const std::optional<std::string> path = args.Get("--timeline")) {
        timeline.open(*path, std::ios::out | std::ios::trunc);
        if (!timeline) {
            Log::Error("--timeline: could not write {}", *path);
            return 2;
        }
        timeline << "# vortex_server timeline: ops <last opSeq> <steady-clock ns at the pass start> <clients joining>\n";
    }
    uint32_t timelineOps = 0;
    size_t timelineJoining = 0;
    auto lastTimelineFlush = std::chrono::steady_clock::now();
    auto writeTimeline = [&](std::chrono::steady_clock::time_point passStart) {
        if (!timeline.is_open()) return;
        const size_t joining = server.JoiningClients();
        if (server.LastOpSeq() != timelineOps || joining != timelineJoining) {
            timelineOps = server.LastOpSeq();
            timelineJoining = joining;
            timeline << "ops " << timelineOps << ' '
                     << std::chrono::duration_cast<std::chrono::nanoseconds>(passStart.time_since_epoch()).count() << ' ' << joining << '\n';
        }
        if (passStart - lastTimelineFlush >= std::chrono::seconds(1)) {
            timeline.flush();
            lastTimelineFlush = passStart;
        }
    };
    double lastWall = wallNow();                 // stage 8: the clock starts here, not before the preload
    double maxPassSeconds = 0.0;                 // wall clock of the server's work in one pass (UDP)
    net::DurationHistogram passBusy;             // ... every pass's, in 0.05 ms buckets (review of stage 8: its p99 beside the
                                                 // ticks'; recheck: a fixed histogram, a listening server runs for days)
    uint32_t slowPasses = 0;                     // ... passes longer than a tick
    // One pass: the server's due ticks (one simulated tick, or what the wall clock gives), then the bots.
    auto pass = [&] {
        const auto passStart = std::chrono::steady_clock::now();
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
            if (server.TickNumber() > before) passBusy.Add(busy);
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
        writeTimeline(passStart);
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

    // The per-chunk hash cache, warmed before anyone joins (a leave then hashes only what changed).
    {
        const auto t0 = std::chrono::steady_clock::now();
        (void)server.Hash();
        Log::Info("Server: window hashed in {:.1f} ms ({} chunk hashes cached)",
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1000.0, server.ChunkHashesComputed());
    }
    // --broken-chunk-candidate (stage 8): the chunk a vortex_bot --broken-chunk may break (tools/soak.sh).
    if (args.Has("--broken-chunk-candidate")) {
        glm::ivec3 c{0};
        std::string why;
        if (net::BrokenChunkCandidate(server, c, &why)) Log::Info("Broken chunk candidate: {},{},{}", c.x, c.y, c.z);
        else Log::Warn("Broken chunk candidate: none ({})", why);
    }
    lastWall = wallNow();
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
        auto walking = [&] {
            return walkAcross && !bots.empty() && (!bots[0].bot || !bots[0].bot->Pilot() || !bots[0].bot->Pilot()->Done()) &&
                   server.TickNumber() < 72000;
        };
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
            if (!bot.bot || !bot.bot->GetSimulation() || bot.bot->GetClient().AppliedOpSeq() != server.LastOpSeq() || !bot.bot->GetClient().Synced())
                return false;
        return true;
    };
    const uint32_t drainStart = server.TickNumber();
    const double drainWall = wallNow();
    // A moving client window (--client-window) may still be generating its far (cave) chunks when the
    // walk ends (a bot's workers, not its ticks, decide how long): its drain goes on while a bot's
    // world has chunks to load, up to two minutes of wall clock.
    // A bot in server terrain mode (stage 7b M-b) receives its whole window as bulk (megabytes, paced):
    // its drain goes on while it is not synced, up to three minutes of wall clock and 30 simulated.
    bool loadingLeft = clientWindow > 0;
    bool terrainLeft = true;
    auto drainMore = [&] {
        if (server.TickNumber() - drainStart < 600) return true;
        if ((server.TickNumber() - drainStart) % 60 == 0) {
            terrainLeft = false;
            for (const BotClient& bot : bots)
                if (bot.bot && bot.bot->GetSimulation() && bot.bot->GetClient().TerrainMode() && !bot.bot->GetClient().Synced()) terrainLeft = true;
        }
        if (terrainLeft && wallNow() - drainWall <= 180.0 && server.TickNumber() - drainStart < 108000) return true;
        if (!loadingLeft || wallNow() - drainWall > 120.0) return false;
        if ((server.TickNumber() - drainStart) % 60 == 0) {
            loadingLeft = false;
            for (const BotClient& bot : bots)
                if (bot.bot && bot.bot->GetSimulation() && bot.bot->GetSimulation()->World().Stats().pendingChunks > 0) loadingLeft = true;
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
    if (drill && server.Casts().size() >= settings.castRecordLimit)
        Log::Warn("Drill: the cast record is full ({} records): the oldest casts were dropped and the drill report below is off",
                  settings.castRecordLimit);
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
        if (!bots[i].bot) continue;              // never connected (reported below)
        Log::Info("{}", net::DescribeBotMovement(*bots[i].bot, tickSeconds));
        Log::Info("{}", net::DescribeBotObjects(*bots[i].bot));
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
    Log::Info("Checks: server: {} hash checks ({} recent, {} modified, {} pristine entries), {} repairs requested / {} sent; {} entity "
              "checks, {} list requests ({} merged, {} lists delayed), {} lists / {:.1f} KiB; checks {:.2f} ms max per peer and tick, {:.1f} "
              "ms in all",
              st.hashChecksSent, st.hashCheckEntries[0], st.hashCheckEntries[1], st.hashCheckEntries[2], st.repairsRequested, st.repairsSent,
              st.entityChecksSent, st.entityListRequests, st.entityListsMerged, st.entityListsDelayed, st.entityListsSent,
              static_cast<double>(st.entityListBytes) / 1024.0, st.maxCheckSeconds * 1000.0, st.checkSeconds * 1000.0);
    if (st.terrainModePeers > 0)
        Log::Info("Terrain: server: {} peer(s) in server terrain mode, {} acks / {:.1f} KiB (entering chunks: {} uniform, {} data, {} packed "
                  "data), {} ChunkData / {:.2f} MiB to them; acks built in {:.2f} ms at most, {:.1f} ms in all (the {} step acks: {:.2f} ms "
                  "at most, {:.3f} ms on average); bulk queues sorted in {:.2f} ms at most, {:.1f} ms in all, {} queued chunks pruned (left "
                  "their window)",
                  st.terrainModePeers, st.terrainAcks, static_cast<double>(st.terrainAckBytes) / 1024.0, st.terrainUniform, st.terrainData,
                  st.terrainHidden, st.terrainChunkData, static_cast<double>(st.terrainChunkBytes) / (1024.0 * 1024.0),
                  st.maxTerrainAckSeconds * 1000.0, st.terrainAckSeconds * 1000.0, st.terrainStepAcks, st.maxTerrainStepAckSeconds * 1000.0,
                  st.terrainStepAcks > 0 ? st.terrainStepAckSeconds * 1000.0 / st.terrainStepAcks : 0.0, st.maxQueueSortSeconds * 1000.0,
                  st.queueSortSeconds * 1000.0,
                  st.queuePruned);
    Log::Info("Ticks: server: {} ticks, {:.2f} ms on average, p50 under {:.2f} ms, p99 under {:.2f} ms, {:.2f} ms at most, {} over 16.7 ms "
              "(wall clock; in-process bots share its cores); {} join ticks {:.2f} ms at most ({} colliders cooked in one at most), {} "
              "first-window ticks {:.2f} ms at most, {} leave ticks {:.2f} ms at most; spawn colliders {} built at start in {:.1f} ms",
              st.ticksTimed, st.ticksTimed > 0 ? st.tickSeconds * 1000.0 / st.ticksTimed : 0.0, st.TickPercentile(0.50) * 1000.0,
              st.TickPercentile(0.99) * 1000.0, st.maxTickSeconds * 1000.0, st.ticksOverBudget, st.helloTicks, st.maxHelloTickSeconds * 1000.0,
              st.maxHelloTickCooks, st.firstWindowTicks, st.maxFirstWindowTickSeconds * 1000.0, st.leaveTicks, st.maxLeaveTickSeconds * 1000.0,
              st.spawnColliders, st.spawnColliderSeconds * 1000.0);
    {
        // Stage 8: the histograms that replace the design's estimates (networking.md "Stage 8 measurements").
        const auto pct = [](const auto& h, double f) { return net::ServerStats::HistogramPercentile(h, f); };
        Log::Info("Server: cast cost per 10 000 work units p50 under {:.1f} ms, p99 under {:.1f} ms, {:.2f} ms at most; cast delay p50 / p99 "
                  "from the histogram above; snapshots p50 under {} B, p99 under {} B, {} B at most, {} bodies at most; bulk in flight to one "
                  "peer at most {:.1f} KiB, largest bulk message {:.1f} KiB",
                  pct(st.castCostHistogram, 0.5) * 0.1, pct(st.castCostHistogram, 0.99) * 0.1, st.maxCastMsPer10k,
                  pct(st.snapshotHistogram, 0.5) * 64, pct(st.snapshotHistogram, 0.99) * 64, st.maxSnapshotBytes, st.maxSnapshotBodies,
                  static_cast<double>(st.maxBulkInFlight) / 1024.0, static_cast<double>(st.maxBulkMessage) / 1024.0);
        std::string kinds;
        static constexpr std::pair<int, const char*> kKinds[] = {{1, "CarveSphere"}, {2, "FillSphere"}, {3, "SwapSphere"}, {4, "FillBox"},
                                                                 {5, "Detach"},    {6, "ClearMask"},    {32, "EntitySpawn"}, {33, "EntityDespawn"},
                                                                 {34, "EntityRest"}, {35, "ObjectModel"}, {36, "EntityWake"}};
        for (const auto& [k, name] : kKinds) {
            if (st.eventsByKind[k] == 0) continue;
            kinds += std::format("{}{} {} / {:.1f} B avg / {} B max", kinds.empty() ? "" : ", ", name, st.eventsByKind[k],
                                 static_cast<double>(st.eventBytesByKind[k]) / st.eventsByKind[k], st.maxEventBytesByKind[k]);
        }
        Log::Info("Server: events by kind: {}", kinds);
    }
    if (memory) logMemory();
    const voxel::WorldStats ws = server.Simulation().World().Stats();
    Log::Info("Server: world {} dense / {} packed chunks, {:.1f} MiB RLE, store {} / {} slots, search errors {}, incomplete ops {}, "
              "RSS {:.0f} MiB (all simulations of this process)",
              ws.denseChunks, ws.packedChunks, static_cast<double>(ws.packedBytes) / (1024.0 * 1024.0),
              server.Simulation().Chunks().UsedCount(), server.Simulation().Chunks().Capacity(), st.searchErrors, st.incompleteOps,
              net::ResidentMiB());

    const voxel::WorldHashResult sh = server.Hash();
    bool ok = !server.Failed() && sh.Complete() && !interrupted;
    // The entity check (stage 6): the same NetIds and models on every side, the same rest state, rest
    // poses within 5 cm and 1 mrad (review of stage 6).
    const std::vector<net::EntityInfo> serverEntities = server.Entities();
    Log::Info("Entities: server {} object(s), {} awake, id hash {:016x}", serverEntities.size(), server.AwakeBodies(),
              net::EntityListHash(serverEntities));
    for (size_t i = 0; i < bots.size(); ++i) {
        if (!bots[i].bot || !bots[i].bot->GetSimulation()) continue;
        const net::EntityComparison c = net::CompareEntities(serverEntities, bots[i].bot->GetClient().Entities());
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
        if (!bot.bot) {
            Log::Error("Client {}: never connected", i);
            ok = false;
            continue;
        }
        const net::Client& client = bot.bot->GetClient();
        sim::Simulation* simulation = bot.bot->GetSimulation();
        const voxel::WorldHashResult ch = client.Hash();
        const net::ClientStats& cs = client.Stats();
        // Stage 7a: compared over the bot's sync box (its window inside the server's) at the same opSeq,
        // once it is synced; for a fixed-window bot that box is the region's window.
        const voxel::WorldHashResult sb = server.Hash(client.SyncWindow());
        const bool equal = simulation && client.Synced() && ch.Complete() && sb.Complete() && ch.hash == sb.hash &&
                           client.AppliedOpSeq() == server.LastOpSeq();
        ok = ok && equal;
        Log::Info("Client {}: {} at opSeq {} / {}, joined at opSeq {}, {} events / {:.1f} KiB, {} duplicates, {} probe mismatches, {} skipped / "
                  "{} unsynced chunks, hash {:016x} {}",
                  i, simulation ? "attached" : "not attached", client.AppliedOpSeq(), server.LastOpSeq(), client.JoinOpSeq(), cs.eventsApplied,
                  static_cast<double>(cs.eventBytes) / 1024.0, cs.duplicateEvents, cs.probeMismatches, cs.skippedChunks, cs.unsyncedChunks, ch.hash,
                  equal ? "equal" : "DIFFERENT");
        Log::Info("{}", net::DescribeBotJoin(*bot.bot, tickSeconds));
        {
            // Event latency (server ticks from an event being made to this bot applying it), for the
            // events made while some bot was joining (connected, its join data not in yet) and for all.
            const auto duringJoin = [&bots](uint32_t made) {
                for (const BotClient& b : bots)
                    if (b.connected && made >= b.connectTick && (b.syncTick == 0 || made < b.syncTick)) return true;
                return false;
            };
            std::vector<uint32_t> joinLat, allLat;
            for (const auto& [opSeq, applied] : bot.bot->Applied()) {
                if (opSeq == 0 || opSeq > madeAt.size()) continue;
                const uint32_t made = madeAt[opSeq - 1];
                const uint32_t latency = static_cast<uint32_t>(applied) - made;
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
        {
            // Stage 7b / 8: the bot's own verdict (net::JudgeBot): no check found a difference no fault
            // made, the injected faults were found and repaired, the store gate, the checks ran. The
            // in-process bots join late in short runs: at least one check of each kind.
            net::BotGates gates;
            const net::BotVerdict v = net::JudgeBot(*bot.bot, gates);
            ok = ok && v.ok;
            std::string reasons;
            for (const std::string& r : v.reasons) reasons += (reasons.empty() ? "" : "; ") + r;
            Log::Info("Checks: bot {}: {}{}{}", i, net::DescribeChecks(client), v.checksNote, v.ok ? "" : " - FAILED: " + reasons);
            if (client.TerrainMode()) {
                // Packing follows the server's (review of stage 7b): over the bot's sync box, a chunk the
                // server shows (Dense) must never be packed here (it would be drawn as a solid block);
                // resident here while packed there is the conservative side (near the player, or shown
                // after an operation near it).
                uint32_t packedShown = 0, residentHidden = 0;
                if (simulation) {
                    const voxel::World& sw = server.Simulation().World();
                    const voxel::World& cw = simulation->World();
                    const net::SyncBox sbx = client.SyncWindow();
                    for (int z = sbx.min.z; z < sbx.Max().z; ++z)
                        for (int y = sbx.min.y; y < sbx.Max().y; ++y)
                            for (int x = sbx.min.x; x < sbx.Max().x; ++x) {
                                const voxel::CellState there = sw.StateOf({x, y, z}), here = cw.StateOf({x, y, z});
                                packedShown += there == voxel::CellState::Dense && here == voxel::CellState::Packed ? 1u : 0u;
                                residentHidden += there == voxel::CellState::Packed && here == voxel::CellState::Dense ? 1u : 0u;
                            }
                }
                if (packedShown > 0) ok = false;
                Log::Info("Terrain: bot {} packing: {} chunk(s) the server shows packed here{}, {} resident here that the server packs", i,
                          packedShown, packedShown > 0 ? " - DRAWN AS SOLID BLOCKS" : "", residentHidden);
                const net::TerrainPeerCost t = server.TerrainCost(client.Slot());
                const double perStep = t.steps > 0 ? static_cast<double>(t.stepBytes) / static_cast<double>(t.steps) / 1024.0 : 0.0;
                const auto avgKiB = [&t](size_t k) {
                    return t.ackSteps[k] > 0 ? static_cast<double>(t.ackStepBytes[k]) / static_cast<double>(t.ackSteps[k]) / 1024.0 : 0.0;
                };
                Log::Info("Terrain: bot {}: {}; server: join ack {:.1f} KiB + {} ChunkData / {:.2f} MiB ({} uniform chunks in the ack), {} "
                          "window steps, ack + data {:.1f} KiB per step on average, {:.1f} KiB at most, {} chunks still queued ({} at most, {} "
                          "outside its window); step acks (events channel) along x {} / {:.2f} KiB on average / {:.2f} KiB at most, along z {} "
                          "/ {:.2f} / {:.2f}, both {} / {:.2f} / {:.2f}; store at most {} slots",
                          i, net::DescribeTerrainMode(client), static_cast<double>(t.joinAckBytes) / 1024.0, t.joinChunks,
                          static_cast<double>(t.joinDataBytes) / (1024.0 * 1024.0), t.joinUniform, t.steps, perStep,
                          static_cast<double>(t.maxStepBytes) / 1024.0, t.queuedChunks, t.maxQueuedChunks, t.queuedOutsideWindow,
                          t.ackSteps[0], avgKiB(0), static_cast<double>(t.maxAckStepBytes[0]) / 1024.0, t.ackSteps[1], avgKiB(1),
                          static_cast<double>(t.maxAckStepBytes[1]) / 1024.0, t.ackSteps[2], avgKiB(2),
                          static_cast<double>(t.maxAckStepBytes[2]) / 1024.0, cs.maxStoreUsed);
            }
        }
        for (const std::string& line : net::DescribeBotWindow(*bot.bot)) Log::Info("{}", line);
        // The server's hash over the bot's sync box beside it (the region's hash for a fixed window).
        hashes += std::format(", client {} {:016x} (server {:016x} over its sync box)", i, ch.hash, sb.hash);
        if (bot.udp)
            Log::Info("Client {} UDP: {} of {} datagrams dropped on receive, RTT {:.1f} ms, {} messages sent / {} received", i,
                      bot.udp->Stats().datagramsDropped, bot.udp->Stats().datagramsReceived, bot.udp->RoundTripSeconds(1) * 1000.0,
                      bot.udp->Stats().messagesSent, bot.udp->Stats().messagesReceived);
    }
    if (serverUdp) {
        // The pass covers the server's ticks and the drill's rebuilds (net::RunDrillStep: FillBox and crates,
        // applied between ticks); Ticks: server: covers only Server::Tick (the casts run there).
        const net::Percentiles pp = passBusy.Summary();
        Log::Info("Server: longest pass {:.1f} ms of server work (ticks and drill edits), {} pass(es) longer than a tick; passes that ran a "
                  "tick: p50 {:.2f} ms, p99 {:.2f} ms over {}",
                  maxPassSeconds * 1000.0, slowPasses, pp.p50 * 1000.0, pp.p99 * 1000.0, pp.count);
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
