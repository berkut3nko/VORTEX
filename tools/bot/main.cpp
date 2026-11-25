// vortex_bot: one bot player in its own process (docs/design/networking.md "Stage 8": the soak).
//
//   vortex_bot --connect HOST:PORT --token HEX (--seconds S | --end-at UNIX_S) [--id N] [--seed S]
//              [--drain-seconds S] [--leave-file PATH] [--leave-wait S] [--parent-pid PID]
//              [--client-window N] [--threads T] [--present]
//              [--stand] [--no-jumps] [--casts N] [--crates N] [--walk-across [--loop]] [--fight]
//              [--broken-chunk X,Y,Z] [--broken-generator]
//              [--loss P] [--delay-ms D] [--jitter-ms J] [--outage-ms A,B]
//              [--down-bytes-per-second R] [--up-bytes-per-second R] [--relay-seed S]
//              [--timeline FILE] [--memory] [--test-undeclared-broken-chunk X,Y,Z] [--test-skip-drain] [--land DIR]
//   vortex_bot --version       (prints the build and exits)
//
// The port must be given (review of stage 8: no default, so a bot never joins the game's default port
// 27015 by accident) and so must an end (--seconds or --end-at: a bot never runs forever).
// --parent-pid PID (Linux): exit as on SIGTERM when that process (the script that started it) is gone.
//
// A net::Bot (the bot of vortex_server's in-process runs: walks, runs, jumps, casts every N ticks, drops
// a crate every N ticks, or walks across the region and back) on the wall clock, over its own UDP
// socket (ENet). With any impairment option its ENet talks to a UdpRelay on 127.0.0.1 that forwards to
// the server: --loss in both directions, --delay-ms / --jitter-ms each way, an outage (both ways, ms
// from the start), a bottleneck down or up (bytes per second, drop-tail queue of 250 ms). Below ENet
// every effect is real for both ENets (resends, round trips, the server's bulk pacer).
//
// Phases: it plays until --seconds after its Welcome or until the wall-clock time --end-at (UNIX
// seconds: a script ends every bot at once); then it stands and drains (net::DrainWatch: applied the
// server's last opSeq, synced, every copy resting, for 1 s; at most --drain-seconds, default 60); with
// --leave-file it logs `Bot: N ready to leave` and keeps draining until that file exists (at most
// --leave-wait s, default 900), so a script lets the bots leave one by one; it leaves once the drain rule
// holds again (at most 15 s later). At exit it logs the game's lines (`Session: slot S applied opSeq A
// of the server's B; world hash H (N chunks) over [box]`, `Session: slot S entities at opSeq A: ...`),
// vortex_server's per-bot lines (`Client N movement / objects / window / walk`, `Join: bot N`, `Checks:
// bot N`, `Terrain: bot N`), and `Latency: bot N` (from the server's --timeline file: same host only),
// `Traffic: bot N`, `Relay: bot N`, `Client N UDP`, `Memory: bot N`; last `Bot: N ok` or `Bot: N
// FAILED: ...` (net::JudgeBot). Injected faults: --broken-chunk X,Y,Z (its generator makes that chunk
// differently: it stands until the chunk is repaired; the coordinate comes from vortex_server
// --broken-chunk-candidate), --broken-generator (another build's terrain: server terrain mode).
// Exit code 0 when its verdict passes (and it was not stopped by a signal, and its drain held when it left),
// 1 otherwise, 2 on a bad command line.

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/prctl.h>
#include <unistd.h>
#endif

import vortex.core;
import vortex.voxel;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    std::atomic<bool> g_Stop{false};
    void OnSignal(int signal) {
        g_Stop = true;
        std::signal(signal, SIG_DFL);
    }

    double Number(const CommandLine& args, const char* name, double fallback) {
        const std::optional<std::string> v = args.Get(name);
        return v ? std::atof(v->c_str()) : fallback;
    }

    /// Steady-clock seconds (the machine's monotonic clock: the server's timeline uses the same one).
    double SteadySeconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    double UnixSeconds() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

    /// What the transport, the relay and the prediction had counted at one moment (the phases' rates).
    struct Snapshot {
        double at = 0.0;
        net::EnetStats enet{};
        net::RelayStats up{}, down{};
        uint64_t ticks = 0, corrections = 0, clockJumps = 0, hardResyncs = 0;
    };
}

int main(int argc, char** argv) {
    const CommandLine args(argc, argv);
    // --version (WP1): which build this is, before anything else starts (tools/install-live.sh never runs it).
    if (args.Has("--version")) {
        std::printf("vortex_bot %s\n", DescribeBuild().c_str());
        return 0;
    }
    Log::Init();
    Log::Info("Build: {}", DescribeBuild());
    const std::optional<std::string> connect = args.Get("--connect");
    const std::optional<std::string> tokenText = args.Get("--token");
    const double seconds = Number(args, "--seconds", 0.0);
    const double endAt = Number(args, "--end-at", 0.0);
    if (!connect || !tokenText || (seconds <= 0.0 && endAt <= 0.0)) {
        std::fprintf(stderr, "usage: vortex_bot --connect HOST:PORT --token HEX (--seconds S | --end-at UNIX_S) [--id N] [--seed S] "
                             "[--drain-seconds S] [--leave-file PATH] [--leave-wait S] [--parent-pid PID] [--client-window N] [--threads T] "
                             "[--present] [--stand] [--no-jumps] [--casts N] [--crates N] [--walk-across [--loop]] [--fight] [--broken-chunk X,Y,Z] "
                             "[--broken-generator] [--loss P] [--delay-ms D] [--jitter-ms J] [--outage-ms A,B] [--down-bytes-per-second R] "
                             "[--up-bytes-per-second R] [--relay-seed S] [--timeline FILE] [--memory]\n");
        return 2;
    }
    std::string host;
    uint16_t port = 0;
    // The port is required (review of stage 8): the game's default port is the one a live session uses.
    if (connect->find(':') == std::string::npos || !net::ParseEndpoint(*connect, net::kDefaultPort, host, port)) {
        Log::Error("--connect expects HOST:PORT (the port is required): '{}'", *connect);
        return 2;
    }
    // --parent-pid (review of stage 8): a script killed with SIGKILL runs no trap; its bots must not keep
    // the port and the cores for the next quarter of an hour.
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
    net::BotSettings bs;
    if (!net::ParseToken(*tokenText, bs.client.token)) {
        Log::Error("--token expects 32 hex digits");
        return 2;
    }
    const uint32_t id = static_cast<uint32_t>(Number(args, "--id", 0));
    bs.id = id;
    bs.seed = static_cast<uint64_t>(Number(args, "--seed", 0));
    bs.client.presentation = args.Has("--present");
    const int32_t window = static_cast<int32_t>(Number(args, "--client-window", 48));
    if (window < 0 || window > net::kMaxClientWindowChunks) {
        Log::Error("--client-window expects 0..{} chunks", net::kMaxClientWindowChunks);
        return 2;
    }
    bs.client.windowChunks = window;
    // --land DIR: the world map this bot bakes from its own files, for a land session (land.md "Decision" 3).
    if (const std::optional<std::string> landDir = args.Get("--land")) {
        voxel::LandSource source;
        std::string landError;
        if (!voxel::LoadLandSource(*landDir, source, landError)) {
            Log::Error("Land: {}", landError);
            return 1;
        }
        const auto land = std::make_shared<const voxel::LandMap>(voxel::BakeLand(source));
        Log::Info("Land: {} baked in {:.2f} s, hash {:016x}", *landDir, land->seconds.total, voxel::LandHash(*land));
        bs.client.landMap = [land] { return land; };
    }
    bs.jobThreads = static_cast<unsigned>(Number(args, "--threads", 2));
    bs.stand = args.Has("--stand");
    bs.pilot.jumps = !args.Has("--no-jumps");
    bs.pilot.castEveryTicks = static_cast<uint32_t>(Number(args, "--casts", 0));
    bs.pilot.casts = bs.pilot.castEveryTicks > 0;
    bs.pilot.crateEveryTicks = static_cast<uint32_t>(Number(args, "--crates", 0));
    bs.pilot.fight = args.Has("--fight");          // [wp-7] fight.md "Bots": aim and cast at the enemies
    bs.walkAcross = args.Has("--walk-across");
    bs.pilot.loopWaypoints = args.Has("--loop");
    if (const std::optional<std::string> b = args.Get("--broken-chunk")) {
        int x = 0, y = 0, z = 0;
        if (std::sscanf(b->c_str(), "%d,%d,%d", &x, &y, &z) != 3) {
            Log::Error("--broken-chunk expects X,Y,Z (a chunk coordinate: vortex_server --broken-chunk-candidate)");
            return 2;
        }
        bs.hasBrokenChunk = true;
        bs.brokenChunk = glm::ivec3(x, y, z);
    }
    bs.brokenGenerator = args.Has("--broken-generator");
    // tools/soak.sh's self-test (SOAK_SELFTEST=1): a fault the bot does not declare, which its verdict and
    // the script must catch. Never for a real run.
    if (const std::optional<std::string> b = args.Get("--test-undeclared-broken-chunk")) {
        int x = 0, y = 0, z = 0;
        if (std::sscanf(b->c_str(), "%d,%d,%d", &x, &y, &z) != 3) {
            Log::Error("--test-undeclared-broken-chunk expects X,Y,Z");
            return 2;
        }
        voxel::MaterialLibrary scratch;
        const uint8_t stone = voxel::RegisterDefaultWorldMaterials(scratch).stone;
        const glm::ivec3 chunk(x, y, z);
        bs.client.wrapGenerator = [chunk, stone](std::shared_ptr<voxel::IWorldGenerator> g) { return net::BreakChunks(std::move(g), {chunk}, stone); };
        Log::Warn("Bot {}: test fault: its generator makes chunk ({}, {}, {}) differently and it does not say so", id, x, y, z);
    }
    if (bs.hasBrokenChunk && bs.brokenGenerator) {
        Log::Error("--broken-chunk and --broken-generator are two faults: one bot takes one");
        return 2;
    }
    const double drainSeconds = Number(args, "--drain-seconds", bs.brokenGenerator ? 180.0 : 60.0);
    const std::optional<std::string> leaveFile = args.Get("--leave-file");
    const double leaveWait = Number(args, "--leave-wait", 900.0);
    const bool memory = args.Has("--memory");
    // tools/soak.sh's self-test: a bot that leaves right after its play phase without draining and still
    // says it is fine (exit 0); the script's comparison must catch it. Never for a real run.
    const bool skipDrain = args.Has("--test-skip-drain");
    if (skipDrain) Log::Warn("Bot {}: test fault: it leaves without draining and exits 0", id);

    // The relay below ENet (any impairment option).
    net::RelaySettings rs;
    rs.serverHost = host;
    rs.serverPort = port;
    rs.up.loss = rs.down.loss = Number(args, "--loss", 0.0);
    rs.up.delaySeconds = rs.down.delaySeconds = Number(args, "--delay-ms", 0.0) / 1000.0;
    rs.up.jitterSeconds = rs.down.jitterSeconds = Number(args, "--jitter-ms", 0.0) / 1000.0;
    rs.down.bytesPerSecond = Number(args, "--down-bytes-per-second", 0.0);
    rs.up.bytesPerSecond = Number(args, "--up-bytes-per-second", 0.0);
    rs.seed = static_cast<uint64_t>(Number(args, "--relay-seed", 0x5e1au + id));
    if (const std::optional<std::string> outage = args.Get("--outage-ms")) {
        double a = 0.0, b = 0.0;
        if (std::sscanf(outage->c_str(), "%lf,%lf", &a, &b) != 2 || b <= a) {
            Log::Error("--outage-ms expects A,B with B > A");
            return 2;
        }
        rs.outages.emplace_back(a / 1000.0, b / 1000.0);
    }
    const bool impaired = rs.up.loss > 0.0 || rs.up.delaySeconds > 0.0 || rs.up.jitterSeconds > 0.0 || rs.down.bytesPerSecond > 0.0 ||
                          rs.up.bytesPerSecond > 0.0 || !rs.outages.empty();
    std::unique_ptr<net::UdpRelay> relay;
    std::string why;
    if (impaired) {
        relay = net::UdpRelay::Start(rs, &why);
        if (!relay) {
            Log::Error("Bot {}: {}", id, why);
            return 2;
        }
        Log::Info("Bot {}: relay on 127.0.0.1:{} to {}:{}: loss {:.1f} %, delay {:.0f} + {:.0f} ms each way{}{}{}", id, relay->Port(), host, port,
                  rs.up.loss * 100.0, rs.up.delaySeconds * 1000.0, rs.up.jitterSeconds * 1000.0,
                  rs.down.bytesPerSecond > 0.0 ? std::format(", down {:.0f} B/s", rs.down.bytesPerSecond) : std::string(),
                  rs.up.bytesPerSecond > 0.0 ? std::format(", up {:.0f} B/s", rs.up.bytesPerSecond) : std::string(),
                  rs.outages.empty() ? std::string()
                                     : std::format(", outage {:.1f}..{:.1f} s", rs.outages[0].first, rs.outages[0].second));
    }
    net::EnetSettings es;
    std::unique_ptr<net::EnetTransport> udp =
        relay ? net::EnetTransport::Connect("127.0.0.1", relay->Port(), es, &why) : net::EnetTransport::Connect(host, port, es, &why);
    if (!udp) {
        Log::Error("Bot {}: {}", id, why);
        return 2;
    }
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    net::Bot bot(bs, *udp);
    net::Client& client = bot.GetClient();
    const double start = SteadySeconds();
    const bool acting = true;
    double welcomeAt = -1.0;
    double peakUp = 0.0, peakDown = 0.0, maxRss = 0.0;
    uint64_t lastUp = 0, lastDown = 0;
    uint64_t joinBulk = 0;                       // bulk received when the join data was in (ClientStats::joinDataTicks)
    bool joinBulkTaken = false;
    double lastSample = start;
    double lastMemory = start;
    auto sample = [&](double now) {
        if (now - lastSample >= 1.0) {
            const net::EnetStats& st = udp->Stats();
            peakUp = std::max(peakUp, static_cast<double>(st.bytesSent - lastUp) / (now - lastSample));
            peakDown = std::max(peakDown, static_cast<double>(st.bytesReceived - lastDown) / (now - lastSample));
            lastUp = st.bytesSent;
            lastDown = st.bytesReceived;
            lastSample = now;
        }
        if (now - lastMemory >= 10.0) {
            maxRss = std::max(maxRss, net::ResidentMiB());
            lastMemory = now;
            if (memory && bot.GetSimulation())
                Log::Info("Memory: bot {}: RSS {:.0f} MiB, store {} / {} slots (at most {})", id, net::ResidentMiB(),
                          bot.GetSimulation()->Chunks().UsedCount(), bot.GetSimulation()->Chunks().Capacity(), client.Stats().maxStoreUsed);
        }
    };
    auto snapshot = [&] {
        Snapshot n;
        n.at = SteadySeconds();
        n.enet = udp->Stats();
        if (relay) {
            n.up = relay->Up();
            n.down = relay->Down();
        }
        const net::PredictionStats& p = client.Prediction();
        n.ticks = p.ticks;
        n.corrections = p.corrections;
        n.clockJumps = p.clockJumps;
        n.hardResyncs = p.hardResyncs;
        return n;
    };
    const Snapshot atStart = snapshot();
    Snapshot atJoinData = atStart;               // when the join data was in (the play phase starts there)
    // One pass on the wall clock: the bot's due ticks (the seconds since the last pass began: review of
    // stage 8, the work inside a pass counts), then the transport.
    auto pass = [&](bool act) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        bot.PassOnClock(SteadySeconds, act);
        const double now = SteadySeconds();
        bot.RecordApplied(now);
        sample(now);
        if (!joinBulkTaken && client.Stats().joinDataTicks != 0) {
            joinBulk = client.Stats().bulkBytes;
            joinBulkTaken = true;
            atJoinData = snapshot();
        }
    };
    auto ended = [&] {
        const net::ClientState st = client.State();
        return st == net::ClientState::Refused || st == net::ClientState::Closed || st == net::ClientState::Failed || bot.AttachFailed();
    };

    // Connect, join, play.
    bool failed = false;
    std::string failure;
    while (!g_Stop && !ended()) {
        pass(acting);
        const double now = SteadySeconds();
        if (welcomeAt < 0.0 && client.State() != net::ClientState::Connecting && client.State() != net::ClientState::Joining)
            welcomeAt = now;
        if (welcomeAt < 0.0 && now - start > 15.0) {
            failure = "no Welcome within 15 s";
            break;
        }
        if (endAt > 0.0 ? UnixSeconds() >= endAt : (welcomeAt >= 0.0 && seconds > 0.0 && now - welcomeAt >= seconds)) break;
    }
    if (!failure.empty() || ended() || g_Stop) {
        failed = true;
        if (failure.empty())
            failure = g_Stop ? "stopped by a signal"
                             : client.State() == net::ClientState::Refused ? std::format("refused ({})", net::ToString(client.RefusalReason()))
                                                                            : "the session ended (closed, failed or not attached)";
    }
    // Stand and drain.
    const double playEnd = SteadySeconds();
    const Snapshot atPlayEnd = snapshot();
    if (!joinBulkTaken) atJoinData = atStart;
    net::DrainWatch drain(1.0);
    bool drained = false;
    double lastDrainNote = playEnd;
    while (!failed && !skipDrain && !g_Stop && !ended() && SteadySeconds() - playEnd < drainSeconds) {
        pass(false);
        if (drain.Update(client, SteadySeconds())) {
            drained = true;
            break;
        }
        // Every 10 s of a drain that does not settle: what holds it (stage 8).
        if (SteadySeconds() - lastDrainNote >= 10.0) {
            lastDrainNote = SteadySeconds();
            Log::Info("Bot {}: draining: {}", id, drain.Describe(lastDrainNote));
            if (!client.Synced())
                for (const std::string& line : net::DescribeBotWindow(bot))
                    if (line.find("is not synced") != std::string::npos) Log::Info("{}", line);
        }
    }
    if (!failed && !skipDrain) {
        if (drained) Log::Info("Bot {}: drained in {:.2f} s at opSeq {}", id, SteadySeconds() - playEnd, client.AppliedOpSeq());
        else Log::Warn("Bot {}: the drain did not settle in {:.0f} s: {}", id, drainSeconds, drain.Describe(SteadySeconds()));
    }
    // Wait for the script's word, then leave once the rule holds again.
    if (!failed && !skipDrain && leaveFile) {
        Log::Info("Bot: {} ready to leave", id);
        const double waitStart = SteadySeconds();
        while (!g_Stop && !ended() && !std::filesystem::exists(*leaveFile) && SteadySeconds() - waitStart < leaveWait) {
            pass(false);
            drain.Update(client, SteadySeconds());
        }
        const double released = SteadySeconds();
        // A fresh hold from the word on (the soak: the bot before this one has just left and its crates'
        // despawns are on their way; a hold that began before the word would let this bot leave without them).
        drain.Reset();
        drained = false;
        while (!g_Stop && !ended() && SteadySeconds() - released < 15.0) {
            pass(false);
            if (drain.Update(client, SteadySeconds())) {
                drained = true;
                break;
            }
        }
        if (!drained) Log::Warn("Bot {}: the drain did not hold again in 15 s after the leave word: {}", id, drain.Describe(SteadySeconds()));
    }
    if (!failed && ended()) {
        failed = true;
        failure = "the session ended before it left";
    }
    // Review of stage 8: a signal in any phase, and a drain that did not hold when it left (the rule the
    // script's comparison depends on), fail the bot.
    if (!failed && g_Stop) {
        failed = true;
        failure = "stopped by a signal";
    }
    if (!failed && !skipDrain && !drained) {
        failed = true;
        failure = leaveFile ? "its drain did not hold again within 15 s of the leave word" : "its drain did not settle";
    }
    // Stage 8: the copies that are not resting at the end (the drain accepts one that stands still).
    {
        std::string list;
        uint32_t count = 0;
        for (const net::EntityInfo& e : client.Entities()) {
            if (e.resting) continue;
            if (++count <= 4) list += std::format("{}{} at ({:.1f}, {:.1f}, {:.1f})", count > 1 ? ", " : "", e.netId, e.position.x, e.position.y, e.position.z);
        }
        if (count > 0) Log::Info("Bot {}: {} object(s) not resting at its exit: {}{}", id, count, list, count > 4 ? ", ..." : "");
    }
    const double connectedSeconds = bot.PlayingSince() > 0.0 ? SteadySeconds() - bot.PlayingSince() : 0.0;

    // Exit lines.
    const double tickSeconds = bot.GetSimulation() ? static_cast<double>(bot.GetSimulation()->FixedTimeStep()) : 1.0 / 60.0;
    if (bot.GetSimulation()) {
        Log::Info("{}", net::SessionHashLine(client));
        Log::Info("{}", net::SessionEntitiesLine(client));
        Log::Info("{}", net::SessionAwakeLine(client));
    }
    Log::Info("{}", net::DescribeBotMovement(bot, tickSeconds));
    Log::Info("{}", net::DescribeBotObjects(bot));
    Log::Info("{}", net::DescribeBotJoin(bot, tickSeconds));
    if (const std::optional<std::string> path = args.Get("--timeline")) {
        std::ifstream in(*path);
        std::stringstream text;
        text << in.rdbuf();
        std::vector<net::TimelineEntry> timeline;
        std::string error;
        if (!net::ParseTimeline(text.str(), timeline, &error)) {
            Log::Error("Latency: bot {}: {}", id, error);
        } else {
            const net::EventLatency l = net::LatencyFromTimeline(timeline, bot.Applied(), bot.PlayingSince());
            const double tickMs = tickSeconds * 1000.0;
            Log::Info("Latency: bot {}: events made during joins: p50 {:.0f} ms, p99 {:.0f} ms ({:.1f} ticks), max {:.0f} ms over {} events; "
                      "quiet (no client joining): p50 {:.0f} ms, p99 {:.0f} ms ({:.1f} ticks), max {:.0f} ms over {}; all events: p50 {:.0f} ms, "
                      "p99 {:.0f} ms ({:.1f} ticks), max {:.0f} ms over {}; {} made before it played (its own load, max {:.0f} ms), {} beyond "
                      "the timeline (wall clock, one host)",
                      id, l.duringJoins.p50, l.duringJoins.p99, l.duringJoins.p99 / tickMs, l.duringJoins.max, l.duringJoins.count, l.quiet.p50,
                      l.quiet.p99, l.quiet.p99 / tickMs, l.quiet.max, l.quiet.count, l.all.p50, l.all.p99, l.all.p99 / tickMs, l.all.max,
                      l.all.count, l.beforePlaying, l.beforePlayingMaxMs, l.unknown);
        }
    }
    net::BotGates gates;
    // Checks every 2 s per kind (120 ticks): at least half of what the connected time allows.
    gates.minHashChecks = static_cast<uint32_t>(std::max(1.0, 0.5 * connectedSeconds / 2.0));
    gates.minEntityChecks = gates.minHashChecks;
    // Review of stage 8: it played as it was told (its pilot kept driving, cast, dropped crates, walked).
    gates.minActedShare = 0.8f;
    gates.minPilotShare = 0.5f;
    gates.maxRejectedCastShare = 0.2f;
    gates.minWalkShare = 0.8f;
    // A walker that snags fails even when its detours carry it on (net::WalkerSnagFailures; the bounds:
    // the game's docs/verify.md "vortex_bot and the soak", net::kWalkerMaxStillSeconds).
    gates.maxStillSeconds = net::kWalkerMaxStillSeconds;
    gates.maxDetourShare = net::kWalkerMaxDetourShare;
    gates.maxConfinedSeconds = net::kWalkerMaxConfinedSeconds;
    const net::BotVerdict v = net::JudgeBot(bot, gates);
    Log::Info("Checks: bot {}: {}{}", id, net::DescribeChecks(client), v.checksNote);
    Log::Info("Terrain: bot {}: {}", id, net::DescribeTerrainMode(client));
    for (const std::string& line : net::DescribeBotWindow(bot)) Log::Info("{}", line);
    {
        // Review of stage 8: rates per phase - join (connect to the join data in), play (from there to
        // the end of play), and the drain and the wait for the leave word - not over the whole life (a bot
        // that waits minutes for the others diluted every rate by that).
        const Snapshot atExit = snapshot();
        const net::ClientStats& cs = client.Stats();
        const net::EnetStats& st = atExit.enet;
        Log::Info("Client {} UDP: RTT {:.1f} ms (smoothed {:.1f}), {} messages sent / {} received", id, udp->RoundTripSeconds(1) * 1000.0,
                  udp->SmoothedRoundTripSeconds(1) * 1000.0, st.messagesSent, st.messagesReceived);
        const auto rate = [](uint64_t a, uint64_t b, double sa, double sb) {
            return sb > sa + 1e-3 ? static_cast<double>(b - a) / (sb - sa) : 0.0;
        };
        const auto phase = [&rate](const Snapshot& a, const Snapshot& b) {
            const auto r = [&](uint64_t x, uint64_t y) { return rate(x, y, a.at, b.at); };
            return std::format("{:.0f} s: up {:.0f} B/s (state {:.0f} / events {:.0f} / bulk {:.0f}), down {:.0f} B/s (state {:.0f} / events "
                               "{:.0f} / bulk {:.0f})",
                               b.at - a.at, r(a.enet.bytesSent, b.enet.bytesSent), r(a.enet.channelBytesSent[0], b.enet.channelBytesSent[0]),
                               r(a.enet.channelBytesSent[1], b.enet.channelBytesSent[1]), r(a.enet.channelBytesSent[2], b.enet.channelBytesSent[2]),
                               r(a.enet.bytesReceived, b.enet.bytesReceived), r(a.enet.channelBytesReceived[0], b.enet.channelBytesReceived[0]),
                               r(a.enet.channelBytesReceived[1], b.enet.channelBytesReceived[1]),
                               r(a.enet.channelBytesReceived[2], b.enet.channelBytesReceived[2]));
        };
        Log::Info("Traffic: bot {}: play {}; join {}; drain and wait {}; peak in 1 s up {:.0f} / down {:.0f} B/s; join data {:.1f} KiB in {:.2f} s "
                  "({:.0f} B/s)",
                  id, phase(atJoinData, atPlayEnd), phase(atStart, atJoinData), phase(atPlayEnd, atExit), peakUp, peakDown,
                  static_cast<double>(joinBulk) / 1024.0, cs.joinDataSeconds,
                  cs.joinDataSeconds > 0.0 ? static_cast<double>(joinBulk) / cs.joinDataSeconds : 0.0);
        const double tick = tickSeconds;
        const uint64_t playTicks = atPlayEnd.ticks - atJoinData.ticks;
        Log::Info("Play: bot {}: {:.0f} s of play from its join data in, {} ticks predicted, {} corrected ({:.2f} per minute of play), clock {} "
                  "jumps / {} resyncs in play; {} corrected while it stood and drained",
                  id, atPlayEnd.at - atJoinData.at, playTicks, atPlayEnd.corrections - atJoinData.corrections,
                  playTicks > 0 ? static_cast<double>(atPlayEnd.corrections - atJoinData.corrections) * 60.0 / (static_cast<double>(playTicks) * tick)
                                : 0.0,
                  atPlayEnd.clockJumps - atJoinData.clockJumps, atPlayEnd.hardResyncs - atJoinData.hardResyncs,
                  atExit.corrections - atPlayEnd.corrections);
        if (relay) {
            const auto wire = [&rate](const Snapshot& a, const Snapshot& b) {
                return std::format("up {:.0f} / down {:.0f} B/s on the wire", rate(a.up.bytesIn, b.up.bytesIn, a.at, b.at),
                                   rate(a.down.bytesIn, b.down.bytesIn, a.at, b.at));
            };
            const net::RelayStats& u = atExit.up;
            const net::RelayStats& d = atExit.down;
            Log::Info("Relay: bot {}: play {}, join {}; up {} datagrams ({} lost, {} in the outage, {} at a full queue, {} strangers, queue {:.0f} "
                      "ms at most); down {} datagrams ({} lost, {} in the outage, {} at a full queue, {} strangers, queue {:.0f} ms at most)",
                      id, wire(atJoinData, atPlayEnd), wire(atStart, atJoinData), u.datagramsIn, u.lost, u.outage, u.queueFull, u.strangers,
                      u.maxQueueSeconds * 1000.0, d.datagramsIn, d.lost, d.outage, d.queueFull, d.strangers, d.maxQueueSeconds * 1000.0);
        }
    }
    maxRss = std::max(maxRss, net::ResidentMiB());
    Log::Info("Memory: bot {}: RSS {:.0f} MiB now, {:.0f} at most (sampled every 10 s); store at most {} slots", id, net::ResidentMiB(), maxRss,
              client.Stats().maxStoreUsed);
    std::string reasons = failure;
    for (const std::string& r : v.reasons) reasons += (reasons.empty() ? "" : "; ") + r;
    const bool ok = skipDrain || (!failed && v.ok);
    if (ok) Log::Info("Bot: {} ok", id);
    else Log::Error("Bot: {} FAILED: {}", id, reasons);
    // Leave: ENet's graceful disconnect (the destructor waits up to 0.5 s for the acknowledgement).
    return ok ? 0 : 1;
}
