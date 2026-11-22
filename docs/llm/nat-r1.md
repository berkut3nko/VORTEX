# [nat-R1] Rendezvous: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/rendezvous|rendezvous]]

| | |
|---|---|
| Stage | `[nat-R1]`: testers outside the house join the owner's home server by its token alone: a Cloudflare Worker + D1 swaps candidates, both sides punch, ENet connects over the proven address |
| Design | [rendezvous.md](../design/rendezvous.md): "Decision in brief", "Flow", "Worker API and D1 schema", "The engine side", "Punch datagram", "Command lines", "Security", "Tests" (gate G20), "Owner steps" |
| Code read at | engine `bafca16` + the `[nat-R0]` pack ([nat-r0.md](nat-r0.md)), game `0e27ea7` |
| Depends on | `[nat-R0]` committed (all three cards of nat-r0.md) |
| Cards | 9 (about 7 d (e) for a frontier model; plan 9-10 card-days) |
| Wire / rules / hashes | no session byte changes, no `kProtocolVersion` bump, `StepPlayer` and `buildHash` unchanged. The Worker's API is versioned on its own (`/v1/`). New gate **G20** (tier `full`). New ctest `Tools.RendezvousWorker`. |

## Before you start

- `git log --oneline | grep '\[nat-R0\]'` shows the three nat-R0 cards in both repositories; the build is green.
- `node --version` prints v22.5 or later (Arch: `nodejs`; `node:sqlite` is needed); `pkg-config --modversion libcurl`
  prints a version (Arch: `curl`). If either is missing, stop and ask the owner for `sudo pacman -S nodejs curl`.
  Never `npm install` anything.
- The code this stage builds on (read the files, do not restate them): `engine/src/net/Traversal.cppm`
  (`Candidate`, `CandidateKinds`, `FormatCandidates`, `ParseCandidates`, `RoomKey`, `HostKey`, `ClientJoinSetup`,
  `ClientTraversal`, `ServerJoin`, `ServerTraversal`, `TraversalLimits`, `TraversalFailure`, `PunchTest::MakeLine`),
  `engine/src/net/Stun.cppm` (`StunProbe`, `NatClass`), `engine/src/net/Address.cppm` (`Endpoint`, `Datagram`,
  `LocalInterfaces`, `IsLanInterface`), `engine/src/net/Enet.cppm` (`EnetTransport::Open / ConnectTo / SendRaw /
  TakeRaw`, `ResolveEndpoint`), `engine/src/net/Punch.cppm` and `internal/Punch.cpp` (`Options`, `Parse`,
  `ResolveStun`, `RunNatTool`), `engine/src/net/Session.cppm` (`Token`, `ParseToken`, `BuildHash`),
  `engine/src/net/Messages.cppm` (`kProtocolVersion`), `vortex::Paths::ExecutableDir()` (`engine/src/core/Paths.cppm`),
  `engine/tools/server/main.cpp` (`fileToken`, `serverUdp`, `pass`, the `openEnded` loop), the game's
  `src/Session.cpp` (`RunSession`), `tools/verify.sh` (`gate_G15`, `run_start`, `game_run`, `need_port`),
  `tools/lib/verify-gates.sh` (`judge_G15`), `tools/lib/lock.sh` (`vortex_copy_tree`), `tools/selftest-verify.sh`.

## Decisions (us, 2026-11-22; the owner may veto)

1. **Files of the Worker**: `engine/tools/rendezvous/worker.mjs` (not `worker.js`: Node reads `.mjs` as an ES module
   without a `package.json`, and Cloudflare deploys it the same), `schema.sql`, `wrangler.toml` (tracked, with a
   `database_id` placeholder the owner replaces; a D1 id is not a secret), `local.mjs`, `test.mjs`. Card 1 amends
   rendezvous.md "Worker API and D1 schema" ("Source:" line).
2. **Candidates travel as the candidate string** of `net::FormatCandidates` (`a.b.c.d:port/kind,...`, `-` for none)
   in requests, in both D1 `cands` columns and in replies, not as JSON objects: one format on both sides, and the
   client's string is what `candHash` covers. Card 1 amends the schema comments.
3. **Requests are JSON, replies are text lines** (`text/plain`), so the C++ side needs no JSON reader. Every request
   body is built with `std::format` from hex digits, integers and the candidate string (no character needs
   escaping). The reply lines are defined in card 1 ("Wire of the Worker").
4. **The host key is stored as SHA-256 over its 16 raw bytes**, 64 hex. For the token `00 01 .. 0f`: room
   `94c0f469fd9e5d1ce662cc248d17ff7a`, host key `f7282e40f4707117ccb8083adbd5a27a`, stored hash
   `a93aa346bd1ff4bdc6b3779969104681545f24951254e477260694aec1a2551d` (computed with Python's `hashlib`).
5. **A live room with another key** (409 `room-held`) makes the server back off and retry like any error; a wait on
   an absent room or with a wrong key gets 404 `no-room`, and the server registers again at once.
6. **STUN name lookups stay blocking at start-up** (`ResolveEndpoint`, as R0 does), before the server's first tick
   and before the game's engine starts, where nothing waits behind them. `vortex_https` runs HTTP only. (The design
   puts the lookups on the short-call thread; this is simpler and stalls nothing.)
7. **`curl_global_init` once** (`std::call_once`) when the first client is made; no global cleanup (the process ends).
8. **The join reply names the join id**, so the client's and the server's `Punch: join <id> ...` lines agree.
9. **Client retries**: a `ServerStale` failure joins again after 5 s with a fresh nonce, at most 4 joins and 60 s
   from the first; 429 `too-many-joins` waits 15 s once; every other failure ends the run (exit 1).
10. **Quota**: a reply with status 429 whose body contains `1027` (Cloudflare's page) is the spent daily quota,
    logged once per streak of failures: `Rendezvous: quota spent until 00:00 UTC`.
11. **`--rendezvous` is opt-in** on `vortex_server`: with `--rendezvous URL`, or bare `--rendezvous` to read
    `rendezvous.url` next to the executable. It needs `--listen` and `--token HEX` or `--new-token` (the session
    files' tokens are in the repository; exit 2 otherwise). The game's `--join TOKEN` reads the file when
    `--rendezvous` is absent. URLs: `https://...`, or `http://127.0.0.1:PORT` / `http://localhost:PORT` (local.mjs).
12. **After a reach the client lingers 1 s** before `ConnectTo`, still answering the server's probes, so the server
    proves its side too (its `Punch: ... reached` line) before the session client owns the transport.
13. **G20's test seam**: `local.mjs --caller-ip` sets `CF-Connecting-IP`; `--server-box` / `--client-box` make it
    append a `route` field (the peer's candidates as aliases in the asker's `vortex_natbox`) to wait and join
    replies. `ServerJoin::route` and the client's route then name where to punch; `candHash` still covers the
    posted string. Real Workers never send a route.
14. **Ports.** G20: UDP and TCP `P20` (the server and local.mjs) and UDP `P20S` (the fake STUN server on
    127.0.0.9): main checkout 27048 / 27049, worktree slot N: B+8 / B+9 (B = 27050 + 10 N). Your own runs in this
    stage: UDP 27065 (server), TCP 27066 (local.mjs), UDP 127.0.0.1:27067 (a STUN address nobody answers), 127.0.0.9:27067
    and 127.0.0.10:27067 (fake STUN, card 7). Never 27015.
15. **`vortex_natbox` is Linux only** (POSIX sockets, 127.0.0.0/8 local); it is a test tool, never shipped.
16. **The Worker's node test is a ctest**, `Tools.RendezvousWorker`, registered when `node` is found.

## Skeleton

New and changed interfaces (card 2 lands all of them with stub bodies; cards 3, 5, 6 fill them). Engine paths.

`src/net/Https.cppm` (new, no `.cpp`):
```cpp
module;
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
export module vortex.net:https;
// docs/design/rendezvous.md "The engine side": the HTTPS client net::Rendezvous talks to. Net has no
// HTTPS of its own (vortex_https implements it with libcurl; tests give a fake): start, poll, cancel.
export namespace vortex::net {
    struct HttpsRequest {
        std::string method = "GET";   ///< "GET", "POST" or "DELETE"
        std::string url;              ///< the whole URL
        std::string body;             ///< sent as is with Content-Type: application/json when not empty
        std::vector<std::pair<std::string, std::string>> headers;   ///< e.g. {"Authorization", "Bearer <hex>"}
        uint32_t timeoutMs = 10000;   ///< the whole transfer
        bool longPoll = false;        ///< runs on the long-poll thread (one at a time); else on the short-call thread
    };
    struct HttpsResponse {
        int status = 0;               ///< the HTTP status; 0 when failed
        std::string body;
        bool failed = false;          ///< no HTTP answer: DNS, TCP, TLS, timeout or cancelled
        std::string error;            ///< what failed ("cancelled" after Cancel)
    };
    using HttpsRequestId = uint32_t;  ///< 0 is never a request
    class HttpsClient {
    public:
        virtual ~HttpsClient() = default;
        /// Queues the request and returns at once.
        virtual HttpsRequestId Start(HttpsRequest request) = 0;
        /// The answer once it came (then forgotten: a second Take returns nullopt).
        virtual std::optional<HttpsResponse> Take(HttpsRequestId id) = 0;
        /// Stops a queued or running request; its answer is {failed, "cancelled"}.
        virtual void Cancel(HttpsRequestId id) = 0;
    };
}
```

`src/net/Rendezvous.cppm` (new) + `src/net/internal/Rendezvous.cpp` (Rendezvous, RendezvousJoin, the helpers) +
`src/net/internal/RendezvousRun.cpp` (RendezvousHost, JoinThroughRendezvous):
```cpp
module;
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
export module vortex.net:rendezvous;
import :address;
import :stun;
import :messages;
import :traversal;
import :https;
import :enet;
// docs/design/rendezvous.md [nat-R1]: the client of the Worker (Rendezvous: the server's room;
// RendezvousJoin: one client join) over an HttpsClient, and the runners the programs call
// (RendezvousHost: vortex_server --rendezvous; JoinThroughRendezvous: Shredborne --join).
export namespace vortex::net {
    /// 32 lower-case hex digits of RoomKey(token) / HostKey(token); 16 of a build hash.
    std::string RoomHex(const Token& token);
    std::string HostKeyHex(const Token& token);
    std::string BuildHex(uint64_t buildHash);
    /// "https://..." or "http://127.0.0.1:PORT" / "http://localhost:PORT"; no trailing '/', no white space, at most 200 characters.
    bool ValidRendezvousUrl(std::string_view url);
    /// The first line of dir/rendezvous.url, trimmed, when ValidRendezvousUrl; else nullopt and `error`.
    std::optional<std::string> ReadRendezvousUrlFile(const std::filesystem::path& dir, std::string& error);

    struct RendezvousSettings {
        std::string url;                       ///< ValidRendezvousUrl
        Token token{};
        uint64_t buildHash = 0;                ///< net::BuildHash / Server::BuildHashValue
        uint16_t protocol = kProtocolVersion;
    };
    inline constexpr uint32_t kRendezvousShortTimeoutMs = 10000;   ///< register, join
    inline constexpr uint32_t kRendezvousWaitTimeoutMs = 30000;    ///< the long poll (the Worker holds it 20 s)
    inline constexpr uint32_t kRendezvousDeleteTimeoutMs = 2000;   ///< the DELETE on exit
    inline constexpr double kRendezvousMaxBackoffSeconds = 20.0;

    enum class RendezvousState : uint8_t { Idle, Registering, Waiting, BackingOff, Closing, Closed };
    const char* ToString(RendezvousState s);   ///< "idle", "registering", "waiting", "backing off", "closing", "closed"

    /// The server's room: registers, long-polls for joins, registers again when its candidates or NAT class
    /// change, backs off on errors, deletes the room on Close. Never blocks: Update starts requests and takes answers.
    class Rendezvous {
    public:
        Rendezvous(HttpsClient& https, RendezvousSettings settings);
        /// Every server pass once the first STUN round settled. `candidates`: this server's (ChooseCandidates).
        void Update(double now, std::span<const Candidate> candidates, NatClass nat);
        /// Joins the long poll brought since the last call, oldest first.
        std::vector<ServerJoin> TakeJoins();
        /// Cancels the long poll and starts the DELETE (2 s limit). Update then only takes its answer.
        void Close(double now);
        RendezvousState State() const { return m_State; }
        bool QuotaSpent() const { return m_QuotaLogged; }
        uint32_t Failures() const { return m_Failures; }
        std::vector<std::string> TakeLog();
    private:
        void Fail(double now, const HttpsResponse& r, std::string_view what);
        HttpsClient& m_Https;
        RendezvousSettings m_Settings;
        std::string m_Room, m_Auth;               ///< RoomHex, "Bearer " + HostKeyHex
        RendezvousState m_State = RendezvousState::Idle;
        std::string m_Registered, m_Pending;      ///< "<cands> <nat>" registered / in flight
        size_t m_PendingCount = 0;
        HttpsRequestId m_Post = 0, m_Wait = 0, m_Delete = 0;
        uint32_t m_After = 0, m_Failures = 0;
        double m_NextTry = 0.0;
        bool m_QuotaLogged = false, m_EverRegistered = false;
        std::vector<ServerJoin> m_Joins;
        std::vector<std::string> m_Log;
    };

    enum class JoinFailure : uint8_t { None, NoRoom, AnotherBuild, IpMismatch, NonceUsed, TooManyJoins, Network, BadReply };
    /// The client's words: "no server has this room (is it running with --rendezvous, and is the token right?)",
    /// "the server runs another build", "the Worker saw another outside address than STUN (a VPN?)",
    /// "the rendezvous refused the join's nonce", "too many joins to this room in a minute",
    /// "the rendezvous cannot be reached", "the rendezvous answered something unexpected"; None: "none".
    const char* ToString(JoinFailure f);

    /// One client join: POST /v1/join, retried after 1 and 2 s when it got no HTTP answer.
    class RendezvousJoin {
    public:
        /// `candidates`: this client's lan and stun ones (the rest are dropped, at most 2 stun); `nonce` non-zero.
        RendezvousJoin(HttpsClient& https, RendezvousSettings settings, std::span<const Candidate> candidates, uint64_t nonce, double now);
        void Update(double now);
        bool Done() const { return m_Done; }
        /// Done without a failure: what ClientTraversal starts from.
        const std::optional<ClientJoinSetup>& Result() const { return m_Result; }
        uint32_t JoinId() const { return m_JoinId; }
        JoinFailure Failure() const { return m_Failure; }
        std::vector<std::string> TakeLog();
    private:
        HttpsClient& m_Https;
        RendezvousSettings m_Settings;
        std::string m_Body, m_Posted;             ///< the request body; the candidate string as posted
        uint64_t m_Nonce = 0;
        HttpsRequestId m_Request = 0;
        uint32_t m_Tries = 0, m_JoinId = 0;
        double m_NextTry = 0.0;
        bool m_Done = false;
        JoinFailure m_Failure = JoinFailure::None;
        std::optional<ClientJoinSetup> m_Result;
        std::vector<std::string> m_Log;
    };

    struct RendezvousHostSettings {
        RendezvousSettings rendezvous;
        std::vector<Endpoint> stunServers;
        CandidateKinds kinds;
        std::optional<Endpoint> given;            ///< --public
        std::vector<LocalInterface> interfaces;   ///< LocalInterfaces()
        uint16_t port = 0;                        ///< the listening port
        uint64_t seed = 1;                        ///< STUN transactions, server nonces
    };
    /// vortex_server --rendezvous: STUN, the room and punching over the listening transport's socket. Logs NAT:, Rendezvous: and Punch: lines.
    class RendezvousHost {
    public:
        RendezvousHost(HttpsClient& https, RendezvousHostSettings settings, double now);
        /// Every server pass, after the transport was polled (the server's Advance).
        void Update(double now, EnetTransport& udp);
        void Close(double now);
        /// The DELETE was answered, or 2 s passed since Close.
        bool Closed(double now);
    private:
        RendezvousHostSettings m_Settings;
        StunProbe m_Stun;
        Rendezvous m_Rendezvous;
        ServerTraversal m_Traversal;
        double m_Start = 0.0, m_ClosedAt = -1.0;
        bool m_Gathered = false;
        std::vector<Candidate> m_Candidates;
        std::vector<Datagram> m_Out;
    };

    struct JoinSettings {
        RendezvousSettings rendezvous;
        std::vector<Endpoint> stunServers;
        CandidateKinds kinds;
        std::vector<LocalInterface> interfaces;
        uint16_t port = 0;                        ///< the transport's port (lan candidates)
        uint64_t seed = 1;
    };
    /// Shredborne --join: STUN on udp's socket, POST /v1/join, punching, 1 s of answering after the reach.
    /// Blocks (a 5 ms loop). The proven server address, or nullopt and `error` (the words of "Timeouts").
    std::optional<Endpoint> JoinThroughRendezvous(HttpsClient& https, EnetTransport& udp, const JoinSettings& settings, std::string& error);
}
```

Changed in `src/net/Traversal.cppm` (card 2):
```cpp
    struct ServerJoin {
        // ... id, nonceClient, candidates, verified as today, then:
        std::string route;   ///< G20 only (local.mjs): where to punch instead of `candidates` (same count); candHash still covers `candidates`
    };
    /// The candidates a side offers (PunchTest's line, R1's register and join): lan (IsLanInterface, at most 4, interface
    /// order, at `port`), then given (server only), then each distinct answered stun mapping (at most 2); at most kMaxCandidates, no address twice.
    std::vector<Candidate> ChooseCandidates(PunchRole role, std::span<const LocalInterface> interfaces, uint16_t port,
                                            const CandidateKinds& kinds, const std::optional<Endpoint>& given,
                                            std::span<const std::optional<Endpoint>> stunMapped);
    /// ToString(f), except ServerStale: "the server has not reached the rendezvous for N s (restarting or backing off)".
    std::string FailureText(TraversalFailure f, double serverAgeSeconds);
```
Changed in `src/net/Punch.cppm` (card 5):
```cpp
    /// --stun HOST[:PORT] (repeatable; none: kDefaultStunServers), --candidates KINDS, --public A.B.C.D:PORT (server only).
    struct NatFlags {
        std::vector<std::string> stun;
        CandidateKinds kinds;
        std::optional<Endpoint> given;
    };
    /// Reads those three flags from argv ("--flag value" or "--flag=value"), ignoring every other argument. false and `error` on a malformed one.
    bool ParseNatFlags(int argc, const char* const* argv, PunchRole role, NatFlags& out, std::string& error);
    /// ENet's lookup of each name (blocking; start-up only), default port 3478; logs `NAT: STUN server NAME is A:P` or a warning.
    std::vector<Endpoint> ResolveStunServers(const std::vector<std::string>& names);
```
`src/net/Net.cppm`: `export import :https;` after `:punch`, then `export import :rendezvous;`.

Other new files: `engine/tools/https/{CMakeLists.txt, Https.cppm, internal/CurlHttps.cpp}` (module `vortex.https`:
`std::unique_ptr<net::HttpsClient> vortex::https::MakeCurlHttpsClient();`), `engine/tools/natbox/{CMakeLists.txt,
main.cpp}`, `engine/tools/rendezvous/{worker.mjs, schema.sql, wrangler.toml, local.mjs, test.mjs}`,
`engine/tests/net/FakeHttps.h`, `engine/tests/net/RendezvousTests.cpp`.

Tests (suite `Rendezvous`, `engine/tests/net/RendezvousTests.cpp`, all written in card 2):
`Rendezvous.RegistersWaitsAndDeletes` (passes in card 2), `Rendezvous.ReRegistersWhenTheOutsideAddressChanges`,
`Rendezvous.BacksOffAndLogsTheSpentQuotaOnce`, `Rendezvous.ARestartWithTheSameTokenKeepsTheRoom`,
`Rendezvous.ReRegisterAndDeleteDoNotWaitForTheLongPoll`, `Rendezvous.AnotherBuildIsRefusedBeforePunching`,
`Rendezvous.AStaleServerIsReportedNotBlamedOnTheNat` (card 3 enables them).

## Card 1: The Worker, its D1 schema, the local server and the node tests

| | |
|---|---|
| Goal | The meeting point exists and is tested on this machine: the exact Worker the owner will deploy runs under Node over SQLite, with every rule of the design's API checked. |
| Depends on | `[nat-R0]` |
| Effort | 1 d (e) |
| Touches | `engine/tools/rendezvous/` (JavaScript, no C++), `engine/tests/CMakeLists.txt` (one ctest); wire: no |

**Read first** (about 25 KiB): rendezvous.md "Worker API and D1 schema" (whole, with "Free-tier budget"),
"Security" ("Spending the free tier"), "Owner steps"; decisions 1-4, 10, 13, 16 above.

**Files**: new `engine/tools/rendezvous/worker.mjs`, `schema.sql`, `wrangler.toml`, `local.mjs`, `test.mjs`;
change `engine/tests/CMakeLists.txt`, `engine/docs/design/rendezvous.md` (decisions 1, 2), ROADMAP (test count).

**Wire of the Worker** (exact; every reply body ends with `\n`, `Content-Type: text/plain`)

| Call | Request | 200 reply |
|---|---|---|
| `POST /v1/host` | header `Authorization: Bearer <32 hex host key>`; body `{"room":"<32 hex>","cands":"<string>","nat":"eim\|symmetric\|unknown","build":"<16 hex>","protocol":<1-65535>}` | `ok registered` |
| `GET /v1/host/wait?room=<32 hex>&after=<id>` | the same header | zero or more lines `join <id> <nonce 16 hex> <verified 0\|1> <cands>` (empty body: none in the wait) |
| `POST /v1/join` | body `{"room":..,"cands":"<string>","nonce":"<16 hex>","build":..,"protocol":..}` | `server <join id> <age s> <nat> <server cands> <verified 0\|1>` |
| `DELETE /v1/host` | the header; body `{"room":".."}` | `ok deleted` |

Errors: one line `error <word>[ <detail>]`: 400 `bad-request <field>` (`json`, `room`, `cands`, `nat`, `build`,
`protocol`, `nonce`, `after`), 401 `no-key`, 413 `too-large`, 404 `no-room`, 404 `not-found` (unknown path or
method), 409 `room-held`, 409 `another-build <the room's build>`, 409 `nonce-used`, 403 `ip-mismatch <caller IP>`,
429 `too-many-joins`, 503 `storage` (a D1 call threw). With `local.mjs --server-box / --client-box`, wait lines and the
join line get one more field: the route (decision 13).

**Steps**
1. `schema.sql`: the two tables and the index of rendezvous.md, with the `cands` comments changed to "the candidate
   string (`a.b.c.d:port/kind,...` or `-`; rooms: lan, stun, given; joins: lan and at most 2 stun), returned byte for
   byte". Nothing else differs.
2. `worker.mjs`: an ES module with `export default { fetch, scheduled }` and these named exports for the tests:
   `parseIpv4`, `isPrivate`, `parseCands`. Write it as below (this is the specification; keep names and SQL):
   ```js
   const ROOM_RE = /^[0-9a-f]{32}$/, HEX16_RE = /^[0-9a-f]{16}$/;
   const ABSENT_AFTER = 120, JOIN_LIFE = 60, JOINS_PER_MINUTE = 4, JOINS_KEPT = 16, MAX_BODY = 2048;
   const reply = (status, body) => new Response(body + '\n', { status, headers: { 'content-type': 'text/plain' } });
   const err = (status, word, detail) => reply(status, detail === undefined ? `error ${word}` : `error ${word} ${detail}`);
   const nowOf = env => (env.NOW ? env.NOW() : Math.floor(Date.now() / 1000));
   export function parseIpv4(s) {           // dotted quad, no leading zeros -> uint32, else null
     const p = String(s).split('.'); if (p.length !== 4) return null;
     let v = 0; for (const x of p) { if (!/^(0|[1-9][0-9]{0,2})$/.test(x) || +x > 255) return null; v = v * 256 + +x; }
     return v;
   }
   export const isPrivate = v => Math.floor(v / 2 ** 24) === 10 || Math.floor(v / 2 ** 20) === 0xac1 || Math.floor(v / 2 ** 16) === 0xc0a8;
   export function parseCands(s, kinds) {   // the candidate string -> [{ip, port, kind}] or null
     if (typeof s !== 'string' || s.length > 256) return null;
     if (s === '-') return [];
     const out = [];
     for (const item of s.split(',')) {
       const m = /^([0-9.]+):([1-9][0-9]{0,4})\/([a-z]+)$/.exec(item); if (!m) return null;
       const ip = parseIpv4(m[1]), port = +m[2];
       if (ip === null || port > 65535 || !kinds.includes(m[3])) return null;
       out.push({ ip, port, kind: m[3] });
     }
     return out.length <= 8 ? out : null;
   }
   async function sha256Hex(hex) { /* crypto.subtle.digest('SHA-256', the 16 bytes of `hex`) as 64 lower-case hex */ }
   const bearer = r => (/^Bearer ([0-9a-f]{32})$/.exec(r.headers.get('authorization') ?? '') ?? [])[1] ?? null;
   async function readJson(r) {             // {value} or {error: Response}
     if (+(r.headers.get('content-length') ?? 0) > MAX_BODY) return { error: err(413, 'too-large') };
     const text = await r.text(); if (text.length > MAX_BODY) return { error: err(413, 'too-large') };
     try { const v = JSON.parse(text); if (v && typeof v === 'object' && !Array.isArray(v)) return { value: v }; } catch {}
     return { error: err(400, 'bad-request', 'json') };
   }
   const badProtocol = p => !Number.isInteger(p) || p < 1 || p > 65535;
   ```
   Handlers (each `async (request, env[, url])`; the checks in this order):
   - `host`: `bearer` else 401 `no-key`; `readJson`; `room` ROOM_RE else 400 `room`; `parseCands(cands, ['lan','stun','given'])`
     not null else 400 `cands`; `nat` one of the three else 400 `nat`; `build` HEX16_RE else 400 `build`; protocol else
     400 `protocol`. `hash = sha256Hex(key)`, `now`. `row = SELECT host_key, last_seen FROM rooms WHERE room = ?`
     (`.first()`). If `row && row.host_key !== hash && row.last_seen >= now - ABSENT_AFTER` -> 409 `room-held`. If
     `row && row.host_key !== hash` (an absent room changes owner): `DELETE FROM joins WHERE room = ?`. Then
     `INSERT INTO rooms (room, host_key, cands, nat, build, protocol, last_seen) VALUES (?, ?, ?, ?, ?, ?, ?) ON CONFLICT (room) DO UPDATE SET host_key = excluded.host_key, cands = excluded.cands, nat = excluded.nat, build = excluded.build, protocol = excluded.protocol, last_seen = excluded.last_seen`
     -> 200 `ok registered`.
   - `wait`: `bearer` else 401; `room` from the query (ROOM_RE else 400 `room`), `after` matching
     `/^(0|[1-9][0-9]{0,9})$/` else 400 `after`. `UPDATE rooms SET last_seen = ? WHERE room = ? AND host_key = ? AND last_seen >= ?`
     with `(now, room, hash, now - ABSENT_AFTER)`; `meta.changes === 0` -> 404 `no-room`. Then `steps = +(env.WAIT_STEPS ?? 20)`,
     `stepMs = +(env.WAIT_STEP_MS ?? 1000)`; for `i` in `0..steps-1`: `SELECT id, nonce, verified, cands FROM joins WHERE room = ? AND id > ? AND created >= ? ORDER BY id LIMIT 16`
     with `(room, after, nowOf(env) - JOIN_LIFE)` (`.all()`); rows -> 200 with one `join ...` line each, joined by
     `\n`; else, unless it was the last step, `await new Promise(r => setTimeout(r, stepMs))`. After the loop: 200 with
     an empty line. At most 1 + 20 = 21 queries.
   - `join`: `readJson`; `room` else 400 `room`; `cands = parseCands(b.cands, ['lan','stun'])`, null or more than 2
     `stun` or a `lan` one not `isPrivate` -> 400 `cands`; `nonce` HEX16_RE and not all zeros else 400 `nonce`; `build`,
     `protocol` as above. `room = SELECT cands, nat, build, protocol, last_seen FROM rooms WHERE room = ?`; missing or
     `last_seen < now - ABSENT_AFTER` -> 404 `no-room`. `room.build !== b.build || room.protocol !== b.protocol` -> 409
     `another-build <room.build>`. `caller = request.headers.get('cf-connecting-ip') ?? ''`, `callerIp = parseIpv4(caller)`;
     if `callerIp !== null`: any `stun` candidate with `ip !== callerIp` -> 403 `ip-mismatch <caller>`, else
     `verified = 1`; otherwise (IPv6 or none) `verified = 0`. `SELECT 1 AS used FROM joins WHERE room = ? AND nonce = ?`
     found -> 409 `nonce-used`. `SELECT COUNT(*) AS n FROM joins WHERE room = ? AND created >= ?` with `now - 60`;
     `n >= JOINS_PER_MINUTE` -> 429 `too-many-joins`. `DELETE FROM joins WHERE room = ? AND (created < ? OR id NOT IN (SELECT id FROM joins WHERE room = ? ORDER BY id DESC LIMIT ?))`
     with `(room, now - JOIN_LIFE, room, JOINS_KEPT - 1)`. `INSERT INTO joins (room, cands, nonce, verified, created) VALUES (?, ?, ?, ?, ?)`
     with `b.cands` exactly as posted -> 200 `server ${meta.last_row_id} ${now - room.last_seen} ${room.nat} ${room.cands} ${verified}`.
   - `remove`: `bearer`; `readJson`; `room`; `DELETE FROM rooms WHERE room = ? AND host_key = ?`; `changes === 0` -> 404
     `no-room`; `DELETE FROM joins WHERE room = ?` -> 200 `ok deleted`.
   - `fetch(request, env)`: route by `new URL(request.url).pathname` and method (`POST /v1/host`, `DELETE /v1/host`,
     `GET /v1/host/wait`, `POST /v1/join`, else 404 `not-found`) inside `try`; a thrown error -> 503 `storage`.
   - `scheduled(event, env)`: `cutoff = now - ABSENT_AFTER`; `DELETE FROM joins WHERE room IN (SELECT room FROM rooms WHERE last_seen < ?)`,
     `DELETE FROM rooms WHERE last_seen < ?`, `DELETE FROM joins WHERE created < ?` (now - JOIN_LIFE).
   Use only `env.DB.prepare(sql).bind(...).first() / .all() / .run()` and plain `?` placeholders (both D1 and the shim
   take them). `.all()` gives `{results}`, `.run()` gives `{meta: {changes, last_row_id}}`, `.first()` a row or null.
3. `wrangler.toml`:
   ```toml
   # rendezvous.md "Owner steps": the owner deploys this Worker with wrangler on their own account.
   name = "vortex-rendezvous"
   main = "worker.mjs"
   compatibility_date = "2025-06-01"
   [[d1_databases]]
   binding = "DB"
   database_name = "vortex-rendezvous"
   database_id = "PASTE-THE-ID-THAT-wrangler-d1-create-PRINTS"
   [triggers]
   crons = ["17 * * * *"]
   ```
4. `local.mjs` (Node only, no package): exports `makeD1(path)` and `makeEnv(options)`, and runs a server when started
   as a program (`import.meta.url === pathToFileURL(process.argv[1]).href`).
   - `makeD1(path = ':memory:')`: `new DatabaseSync(path)` from `node:sqlite`, `exec` of `schema.sql` (read next to
     the module), and an object `{ queries: 0, prepare(sql) }` whose statements implement `bind(...args)` (returns the
     statement), `async first()` (`get(...args) ?? null`), `async all()` (`{ results: all(...args), success: true }`),
     `async run()` (`{ success: true, meta: { changes: Number(r.changes), last_row_id: Number(r.lastInsertRowid) } }`);
     each of the three adds 1 to `queries`.
   - `makeEnv({db, waitSteps, waitStepMs, now})` -> `{ DB: makeD1(db), WAIT_STEPS, WAIT_STEP_MS, NOW: now }`
     (`now` a function of seconds or undefined).
   - The program: flags `--port N` (required, TCP on 127.0.0.1 only), `--db FILE`, `--caller-ip A.B.C.D`,
     `--server-box 127.0.0.1:C`, `--client-box 127.0.0.1:C`, `--wait-steps N`, `--wait-step-ms N`,
     `--parent-pid PID` (exit when `process.kill(pid, 0)` throws, checked every second). A bad flag prints
     `usage: node local.mjs --port N [...]` and exits 2. Each request: read the body, build a `Request` for
     `http://127.0.0.1:<port><req.url>` with the method, the headers and the body (none for GET), set
     `cf-connecting-ip` to `--caller-ip` or the socket's address (without `::ffff:`), `await worker.fetch(request, env)`,
     then, with boxes given and status 200: for `GET /v1/host/wait`, append ` <route>` to each `join` line, the
     route being its `cands` mapped through `--server-box`; for `POST /v1/join`, append ` <route>` of the server
     `cands` mapped through `--client-box`. Mapping one `ip:port/kind`: send the UDP text `alias ip:port` to the box,
     await the reply `alias ip:port 127.0.0.1:Q` (1 s limit, else 503 `error storage box`), and write
     `127.0.0.1:Q/kind`; `-` stays `-`. Log `Rendezvous (local): listening on http://127.0.0.1:<port>` once and
     `Rendezvous (local): <METHOD> <path> -> <status>` per request (no bodies: they hold keys).
5. `test.mjs`: `node:test` + `node:assert/strict`; each test makes a fresh `makeEnv({ waitSteps: 3, waitStepMs: 10, now: () => clock })`
   with a variable `clock` it moves, and calls `worker.fetch(new Request(url, init), env)` (headers
   `cf-connecting-ip` set by the test). Token 00..0f (decision 4). The tests, by name:
   - `a server registers, updates with its key, and another key is refused while the room is live`: POST host -> 200;
     again with other cands -> 200 and `SELECT cands` shows the new string; with another key -> 409 `error room-held`.
   - `the stored key is the SHA-256 of its 16 bytes`: after a register, `host_key` = `a93aa346...551d` (decision 4).
   - `a room not seen for 120 s is absent`: clock + 121 -> join 404 `error no-room`; wait 404; another key may
     register (200) and the old joins are gone.
   - `wait returns new joins at once, else an empty body, with at most 21 queries`: with `waitSteps: 20, waitStepMs: 1`:
     a join then a wait -> one `join 1 <nonce> 1 <cands>` line; a second wait with `after=1` -> body `"\n"`; the shim's
     `queries` grew by at most 21 during that wait.
   - `join: 404, 409 another build, 409 reused nonce, 429 after 4 in a minute`: exact bodies, e.g.
     `error another-build 00000000000000ab`; the 5th join in 60 s gives 429, after clock + 61 one more is 200.
   - `join: 403 names the caller when a stun candidate is another IPv4`: cands `203.0.113.20:6000/stun`, caller
     `198.51.100.7` -> 403 `error ip-mismatch 198.51.100.7`.
   - `join: an IPv6 caller is stored unverified`: caller `2001:db8::5` -> 200 ending ` 0`, and the wait line has
     verified `0`.
   - `join: only lan and at most 2 stun, lan private, the string returned byte for byte`: 3 stun -> 400 `cands`;
     `given` -> 400; lan `8.8.8.8` -> 400; a valid string comes back unchanged in the wait line.
   - `validation`: a body of 2049 bytes -> 413; bad JSON -> 400 `json`; room of 31 hex -> 400 `room`; nonce zeros ->
     400 `nonce`; no Authorization on host -> 401.
   - `delete needs the key and removes the room and its joins`: wrong key -> 404; right key -> 200, then join -> 404
     and `SELECT COUNT(*) FROM joins` is 0.
   - `a room keeps at most 16 joins and joins older than 60 s are dropped`: 16 rooms' worth via clock steps of 16 s
     (4 per minute) -> never more than 16 rows; rows with `created < now - 60` are gone after the next insert.
   - `the cron removes rooms absent for 120 s`: `worker.scheduled({}, env)` after clock + 121 -> both tables empty.
6. `engine/tests/CMakeLists.txt`, at the end:
   ```cmake
   # rendezvous.md "Tests": the Worker over node:sqlite (decision 16 of docs/llm/nat-r1.md).
   find_program(VORTEX_NODE node)
   if(VORTEX_NODE)
       add_test(NAME Tools.RendezvousWorker COMMAND "${VORTEX_NODE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/../tools/rendezvous/test.mjs")
       set_tests_properties(Tools.RendezvousWorker PROPERTIES TIMEOUT 60)
   endif()
   ```
7. rendezvous.md "Worker API and D1 schema": the "Source:" line names `worker.mjs`, `schema.sql`, `wrangler.toml`
   (placeholder id), `local.mjs`, `test.mjs`; the two `cands` comments as in step 1; one sentence after the API table:
   "Replies are text lines (`text/plain`); docs/llm/nat-r1.md card 1 gives their exact form." Update ROADMAP's test
   count (N + 1, M unchanged).

**Checks**
1. `node --test engine/tools/rendezvous/test.mjs` - all pass
2. `node engine/tools/rendezvous/local.mjs --port 27066 &` then `curl -s -X POST http://127.0.0.1:27066/v1/join -d '{}'`
   prints `error bad-request room`; stop it
3. `DEPS_FROM=~/Projects/Shredborne/build/_deps tools/build.sh` (re-configures), `tools/ctest.sh -R 'Tools\.RendezvousWorker'`
   passes, `tools/ctest.sh` all pass, `tools/doc-check.sh --build build` PASS

**Must not break**: no `npm`, no `package.json`, no dependency; local.mjs binds 127.0.0.1 only; logs never print a
request body or the Authorization header.

**Done when**: [ ] the node tests pass, also as ctest; [ ] the replies are exactly the table's; [ ] the design's
"Source:" line and comments are amended.

**Traps**
- `node:sqlite` returns `COUNT(*)` as a number; `.get()` returns `undefined` for no row: turn it into `null`.
- Do not read `Date.now()` anywhere but `nowOf`: the tests move `NOW`.
- The `NOT IN (... LIMIT ?)` delete keeps the 15 newest, so after the insert there are at most 16.

## Card 2: The C++ skeleton, the fake HTTPS client, and a room that registers, waits and deletes

| | |
|---|---|
| Goal | The server side can hold a room on the Worker (register, long poll, joins, delete), proven against a fake HTTPS client; every interface of the stage exists. |
| Depends on | card 1 (the wire table) |
| Effort | 1 d (e) |
| Touches | `vortex.net` (`:https`, `:rendezvous` new; `:traversal` changed); wire: no |

**Read first** (about 60 KiB): this pack's "Skeleton" and card 1's "Wire of the Worker"; `engine/src/net/Traversal.cppm`;
`engine/src/net/internal/Traversal.cpp` (`PunchTest::MakeLine`, `RoomKey`, `HostKey`, `ToString(TraversalFailure)`);
`engine/src/net/Net.cppm`; `engine/tests/net/TraversalTests.cpp` lines 1-60 (helpers).

**Files**
- new `engine/src/net/Https.cppm`, `engine/src/net/Rendezvous.cppm`, `engine/src/net/internal/Rendezvous.cpp`,
  `engine/src/net/internal/RendezvousRun.cpp` (stubs only in this card: `RendezvousHost` members do nothing,
  `Closed` returns true, `JoinThroughRendezvous` sets `error = "not built yet"` and returns nullopt, each with the
  comment `// [nat-R1] card 5 fills this` or `card 6`).
- change `engine/src/net/Net.cppm` (two `export import` lines), `engine/src/net/Traversal.cppm` +
  `internal/Traversal.cpp` (`ServerJoin::route`, `ChooseCandidates`, `FailureText`).
- new `engine/tests/net/FakeHttps.h`, `engine/tests/net/RendezvousTests.cpp`. CMake globs both directories: no list
  to edit.

**Steps**
1. Write the two `.cppm` files exactly as the Skeleton; add the stubs. Build.
2. `ChooseCandidates`: move the body of `PunchTest::MakeLine`'s candidate part into it (lan: `kinds.lan`, each
   interface passing `IsLanInterface`, at most 4, at `port`; given: `role == Server && kinds.given && given`; stun:
   `kinds.stun`, each `stunMapped` entry that has a value, skipping an address already listed, at most 2; never more
   than `kMaxCandidates`). `MakeLine` calls it with `m_Stun.Mapped()`. The nat-R0 tests must pass unchanged.
3. `FailureText(f, age)`: `ServerStale` -> `std::format("the server has not reached the rendezvous for {:.0f} s (restarting or backing off)", age)`;
   else `ToString(f)`.
4. `ServerJoin::route`: in `ServerTraversal::AddJoin`, parse `join.route` when not empty (`ParseCandidates`; false or
   a different count than `candidates` -> `BadCandidates`) and take the targets from it, applying the same kind
   rules (lan only private and in a subnet, at most 2 stun, others dropped); `candHash` stays over `join.candidates`.
5. Helpers in `internal/Rendezvous.cpp`: `RoomHex` / `HostKeyHex` (lower-case hex of `RoomKey` / `HostKey`),
   `BuildHex` (`std::format("{:016x}", h)`), `ValidRendezvousUrl` (the rule of the Skeleton: starts with `https://`
   followed by at least one character, or exactly `http://127.0.0.1:` / `http://localhost:` followed by 1-5 digits;
   no `' '`, `'\t'`, `'\n'`; not ending in `'/'`; size 9-200), `ReadRendezvousUrlFile` (missing file: "no
   rendezvous.url next to the program"; trim spaces, `\r`, `\n`; invalid: "rendezvous.url does not hold an https://
   URL").
6. `Rendezvous` (this card: the path without errors). Constructor: `m_Room = RoomHex(token)`, `m_Auth = "Bearer " + HostKeyHex(token)`.
   `Update(now, candidates, nat)`:
   1. If `m_State == Closing`: if `m_Delete` and `Take(m_Delete)` has an answer: log
      `Rendezvous: room <first 8 hex> deleted` on 200, else `Rendezvous: the delete got <status | error>`;
      `m_State = Closed`. Return. If `Closed`: return.
   2. Take the register answer (`m_Post`): 200 -> `m_Registered = m_Pending`, `m_Failures = 0`,
      `m_QuotaLogged = false`; log `Rendezvous: room <8 hex> registered, <m_PendingCount> candidates` the first time
      (`m_EverRegistered`), later `Rendezvous: registered again, <n> candidates (the candidates changed)`;
      `m_State = Waiting`. Anything else -> `Fail(now, r, "register")` (card 3).
   3. Take the wait answer (`m_Wait`): 200 -> for each non-empty line of the body: split on spaces; `join`, then id
      (`std::from_chars`, > 0), nonce (16 hex, non-zero), verified (`0`/`1`), cands, optional route; a malformed line
      is skipped with `Rendezvous: a join line was malformed`; a join with `id <= m_After` is skipped; else push a
      `ServerJoin{id, nonce, cands, verified == 1, route}` and `m_After = id`. Then `m_Failures = 0`. 404 ->
      log `Rendezvous: the Worker no longer has the room; registering again`, `m_Registered.clear()`,
      `m_State = Registering`. Anything else -> `Fail(now, r, "wait")`.
   4. `desired = FormatCandidates(candidates) + " " + ToString(nat)`. If `m_Post == 0 && now >= m_NextTry && desired != m_Registered`:
      start `POST <url>/v1/host` with body
      `{"room":"<room>","cands":"<FormatCandidates>","nat":"<eim|symmetric|unknown>","build":"<BuildHex>","protocol":<n>}`,
      header `Authorization`, `timeoutMs = kRendezvousShortTimeoutMs`, `longPoll = false`; `m_Pending = desired`,
      `m_PendingCount = candidates.size()`; if nothing is registered yet, `m_State = Registering`.
   5. If `m_Wait == 0 && !m_Registered.empty() && now >= m_NextTry`: start `GET <url>/v1/host/wait?room=<room>&after=<m_After>`,
      the header, `timeoutMs = kRendezvousWaitTimeoutMs`, `longPoll = true`; `m_State = Waiting`.
   `Close(now)`: if `m_Wait` -> `Cancel(m_Wait)`, `m_Wait = 0`; start `DELETE <url>/v1/host` with body
   `{"room":"<room>"}`, the header, `timeoutMs = kRendezvousDeleteTimeoutMs`; `m_State = Closing`.
   `TakeJoins` / `TakeLog` swap their vectors out.
7. `FakeHttps.h` (test helper; the class is `FakeHttpsClient`, not a name of the engine):
   ```cpp
   struct FakeRequest { net::HttpsRequestId id = 0; net::HttpsRequest request; bool cancelled = false, taken = false; std::optional<net::HttpsResponse> response; };
   class FakeHttpsClient final : public net::HttpsClient {
   public:
       std::vector<FakeRequest> requests;      // in Start order
       net::HttpsRequestId Start(net::HttpsRequest r) override;   // push {++next, r}; return the id
       std::optional<net::HttpsResponse> Take(net::HttpsRequestId id) override;   // the response once, then taken = true
       void Cancel(net::HttpsRequestId id) override;   // unanswered: cancelled = true, response {0, "", true, "cancelled"}
       void Respond(net::HttpsRequestId id, int status, std::string body);
       void Fail(net::HttpsRequestId id, std::string error);   // {0, "", true, error}
       FakeRequest* Last();                    // nullptr when none
       std::string Header(const FakeRequest& q, std::string_view name) const;
   private:
       net::HttpsRequestId next = 0;
   };
   ```
8. `RendezvousTests.cpp`: write all seven tests (below); the six that card 3 makes pass are named `DISABLED_...`.
   Shared setup: token bytes `0..15`, `settings{url "https://rv.example", token, buildHash 0xab, protocol 10}`, server
   candidates `{{Ipv4(203,0,113,10), 5000}, Stun}`, `NatClass::Eim`.

**Tests** (`engine/tests/net/RendezvousTests.cpp`)
- `Rendezvous.RegistersWaitsAndDeletes` (passes now): `Update(0.0)` -> 1 request: `POST`,
  `https://rv.example/v1/host`, `longPoll` false, `timeoutMs` 10000, header `Authorization` =
  `Bearer f7282e40f4707117ccb8083adbd5a27a`, body exactly
  `{"room":"94c0f469fd9e5d1ce662cc248d17ff7a","cands":"203.0.113.10:5000/stun","nat":"eim","build":"00000000000000ab","protocol":10}`.
  `Respond(1, 200, "ok registered\n")`; `Update(0.1)` -> `State() == Waiting`; request 2 is `GET
  https://rv.example/v1/host/wait?room=94c0f469fd9e5d1ce662cc248d17ff7a&after=0`, `longPoll` true, `timeoutMs` 30000;
  the log holds `Rendezvous: room 94c0f469 registered, 1 candidates`. `Respond(2, 200, "join 7 00000000000000aa 1 203.0.113.20:6000/stun\n")`;
  `Update(0.2)` -> `TakeJoins()` is one join `{7, 0xaa, "203.0.113.20:6000/stun", verified true, route ""}`; request
  3 is the wait with `after=7`. `Close(0.3)` -> request 3 `cancelled`; request 4 is `DELETE https://rv.example/v1/host`,
  body `{"room":"94c0f469fd9e5d1ce662cc248d17ff7a"}`, `timeoutMs` 2000; `State() == Closing`.
  `Respond(4, 200, "ok deleted\n")`; `Update(0.4)` -> `Closed`; log `Rendezvous: room 94c0f469 deleted`.
- `Rendezvous.ReRegistersWhenTheOutsideAddressChanges`: registered and waiting (request 2 in flight); `Update(1.0)`
  with candidate `203.0.113.11:5001/stun` -> request 3 is a POST whose body has `"cands":"203.0.113.11:5001/stun"`;
  request 2 is not cancelled. Answer 200 -> log `Rendezvous: registered again, 1 candidates (the candidates changed)`.
  The same once more with only the NAT class changed to `Symmetric` (body `"nat":"symmetric"`). Same candidates and
  class again: no new request.
- `Rendezvous.BacksOffAndLogsTheSpentQuotaOnce`: step `t` from 0 by 0.25 to 80; after each `Update(t)` answer any new
  unanswered request with `Fail(id, "connection refused")`. Starts of the POSTs: gaps of exactly 1.25, 2.25, 4.25,
  8.25, 16.25, 20.25, 20.25 s (backoff 1, 2, 4, 8, 16, 20, 20 s plus the 0.25 s step in which the failure is taken;
  0.25 is exact in binary, so compare with `EXPECT_DOUBLE_EQ`). Then the next three POSTs are answered `429` with body
  `error code: 1027` -> exactly one log line `Rendezvous: quota spent until 00:00 UTC`, `QuotaSpent()` true; the next
  POST gets 200 -> `Waiting`, `Failures() == 0`, `QuotaSpent()` false; the wait then gets `429` `error code: 1027` ->
  a second quota line (a new streak): 2 in total.
- `Rendezvous.ARestartWithTheSameTokenKeepsTheRoom`: two `Rendezvous` objects with the same settings, each
  `Update(0.0)`: equal POST bodies and equal `Authorization` headers; a third with token bytes `1..16`: another room in
  the body and another header. (Nothing random enters the keys; the Worker side is card 1's test.)
- `Rendezvous.ReRegisterAndDeleteDoNotWaitForTheLongPoll`: registered, the wait (request 2) never answered. A
  changed candidate list starts the POST in the same `Update` (request 3, `longPoll` false) while request 2 stays
  open; answered 200. `Close(2.0)` starts the DELETE (`longPoll` false) in the same call and cancels request 2;
  answered 200 -> `Closed`, though the fake never answered the wait itself.
- `Rendezvous.AnotherBuildIsRefusedBeforePunching` (card 3): `RendezvousJoin j(fake, settings, {lan 192.168.1.20:6000, stun 203.0.113.20:6000}, 0x99, 0.0)`;
  request 1 is `POST https://rv.example/v1/join`, no `Authorization`, body
  `{"room":"94c0f469fd9e5d1ce662cc248d17ff7a","cands":"192.168.1.20:6000/lan,203.0.113.20:6000/stun","nonce":"0000000000000099","build":"00000000000000ab","protocol":10}`.
  `Respond(1, 409, "error another-build 00000000000000cd\n")`; `Update(0.1)` -> `Done()`, `Failure() == AnotherBuild`,
  `Result()` empty, the log has `Rendezvous: the server runs another build (00000000000000cd; this is 00000000000000ab)`;
  no further request.
- `Rendezvous.AStaleServerIsReportedNotBlamedOnTheNat` (card 3): a join answered
  `server 3 40 eim 203.0.113.10:5000/stun 1` -> `JoinId() == 3`, `Result()->serverAgeSeconds == 40`,
  `Result()->server` = that one stun candidate, `Result()->candidates` = the posted string. A `ClientTraversal(*Result(), {}, 0.0)`
  updated from 0 to 10.1 s in 0.1 steps with no datagram in -> `Failed`, `Failure() == ServerStale`, and
  `FailureText(ServerStale, 40)` is `the server has not reached the rendezvous for 40 s (restarting or backing off)`.
  The same with `server 4 5 eim ...` -> `NoAnswer` and `the server did not answer the join; try again`.

**Checks**: build exit 0, no warning; `tools/ctest.sh -R 'Rendezvous\.|Traversal\.' --output-on-failure` all pass (6
disabled); `tools/ctest.sh`; doc-check PASS (test count: the M of doc-check counts `TEST(` macros, disabled ones too).

**Must not break**: the nat-R0 tests (`ChooseCandidates` is a move, not a change); `Rendezvous.KeysOfAKnownToken`;
Net stays free of HTTPS (no curl header anywhere under `engine/src`).

**Done when**: [ ] every Skeleton declaration compiles; [ ] `Rendezvous.RegistersWaitsAndDeletes` passes; [ ] six
`DISABLED_` tests are in the file.

**Traps**
- `HttpsResponse` failed answers have `status == 0`: test `failed` first.
- The wait's `after` is the highest id taken so far, not the number of joins.
- Keep the request bodies byte-exact (no spaces in the JSON): the tests compare strings.

## Card 3: Errors, re-registering, and the client's join

| | |
|---|---|
| Goal | A home server survives a flaky line and a spent quota without losing its room, and a client learns why it cannot join (another build, a stale server) before any punching. |
| Depends on | card 2 |
| Effort | 1 d (e) |
| Touches | `vortex.net` (`:rendezvous`); wire: no |

**Read first** (about 45 KiB): rendezvous.md "Flow" ("Timeouts", "Errors"); `engine/src/net/Rendezvous.cppm`;
`engine/src/net/internal/Rendezvous.cpp`; `engine/tests/net/RendezvousTests.cpp`.

**Files**: change `engine/src/net/internal/Rendezvous.cpp`, `engine/tests/net/RendezvousTests.cpp` (remove the six
`DISABLED_` prefixes), ROADMAP (test count).

**Steps**
1. `Rendezvous::Fail(now, r, what)`: `++m_Failures`; `delay = std::min(kRendezvousMaxBackoffSeconds, double(1u << std::min(m_Failures - 1, 5u)))`
   (1, 2, 4, 8, 16, 20, 20 ...); `m_NextTry = now + delay`; `m_State = BackingOff`. If `r.status == 429` and
   `r.body` contains `1027`: when `!m_QuotaLogged` log `Rendezvous: quota spent until 00:00 UTC` and set it; log
   nothing else. Otherwise log `Rendezvous: <what> failed (<HTTP <status>: <first line of body> | r.error>); trying again in <delay> s`
   (`{:.0f}`). A 409 `room-held` is such a failure (decision 5). The request id that failed is cleared
   (`m_Post` or `m_Wait` = 0) by the caller before `Fail`.
2. Re-registering on change is step 4 of card 2's `Update`; make sure a POST never waits for the wait: no check of
   `m_Wait` in step 4.
3. `RendezvousJoin` constructor: keep candidates of kind `Lan` and at most 2 of kind `Stun`, in their order;
   `m_Posted = FormatCandidates(kept)`; `m_Body = {"room":"<RoomHex>","cands":"<m_Posted>","nonce":"<016x>","build":"<BuildHex>","protocol":<n>}`;
   start `POST <url>/v1/join` (`kRendezvousShortTimeoutMs`, no Authorization); `m_Tries = 1`.
4. `RendezvousJoin::Update(now)`:
   1. Done: return. A request in flight with no answer: return. No request and `now >= m_NextTry`: start it again
      (`++m_Tries`) and return.
   2. Failed answer (no HTTP): if `m_Tries < 3` -> `m_NextTry = now + (m_Tries == 1 ? 1.0 : 2.0)`, `m_Request = 0`,
      log `Rendezvous: the join got no answer (<error>); trying again`; else done with `Network`, log
      `Rendezvous: the join got no answer (<error>)`.
   3. 200: parse the first line: `server`, id (> 0), age (unsigned integer seconds), nat (`eim` / `symmetric` /
      `unknown`), cands (`ParseCandidates`), verified (`0` / `1`), optional route (`ParseCandidates`, same count).
      Malformed -> done with `BadReply` (log the line, cut to 200 characters). Else `m_JoinId = id`;
      `m_Result = ClientJoinSetup{token, nonce, m_Posted, route.empty() ? cands : route, double(age)}`; done. If
      verified is 0, log `Rendezvous: the Worker saw IPv6; the HTTPS client should force IPv4`.
   4. Other statuses, by the first word after `error`: 404 -> `NoRoom`; 409 `another-build X` -> `AnotherBuild`, log
      `Rendezvous: the server runs another build (X; this is <BuildHex>)`; 409 `nonce-used` -> `NonceUsed`; 403
      `ip-mismatch IP` -> `IpMismatch`, log `Rendezvous: the Worker saw this machine as IP, STUN as <first stun candidate's IP or "-">: a VPN or a second outside address`;
      429 with `1027` -> `Network` with the quota line; 429 otherwise -> `TooManyJoins`; anything else -> `BadReply`.
      All are done.
5. `ToString(JoinFailure)`: the words of the Skeleton comment.
6. Remove the six `DISABLED_` prefixes; all seven tests pass. Update the test count.

**Checks**: build; `tools/ctest.sh -R 'Rendezvous\.' --output-on-failure` (8 pass, with `KeysOfAKnownToken`);
`tools/ctest.sh`; doc-check PASS.

**Must not break**: the backoff never exceeds 20 s (a backed-off server must still see a join within its 60 s life);
no `sleep` or blocking wait in `Rendezvous` or `RendezvousJoin`.

**Done when**: [ ] all `Rendezvous.*` tests pass, none disabled; [ ] every log line above is produced by a test or
named in the report as untested.

**Traps**: `1u << 31` overflows nothing here, but `m_Failures - 1` on 0 does: `Fail` increments first. A 429 with
`1027` on a join is the quota, not the room's join limit.

## Card 4: `vortex_https`: libcurl on two threads

| | |
|---|---|
| Goal | The programs can talk HTTPS to the Worker without ever stalling a server tick or a game frame. |
| Depends on | card 2 (`net::HttpsClient`) |
| Effort | 1 d (e) |
| Touches | new library `vortex_https` (module `vortex.https`); `engine/CMakeLists.txt` (a configure check); wire: no |

**Read first** (about 20 KiB): rendezvous.md "The engine side" (the `vortex_https` bullets); `engine/src/net/Https.cppm`;
`engine/CMakeLists.txt` (`vortex_assert_no_network` and around); `engine/tools/server/CMakeLists.txt`; the game's
`CMakeLists.txt` (`target_link_libraries(Shredborne ...)`).

**Files**
- new `engine/tools/https/CMakeLists.txt`:
  ```cmake
  # vortex_https (rendezvous.md "The engine side"): net::HttpsClient over the system's libcurl (a recorded exception
  # to pinned dependencies: it is the system's TLS). Linked only by vortex_server and the game, never by Vortex::Net.
  find_package(CURL REQUIRED)
  add_library(vortex_https STATIC)
  target_sources(vortex_https
      PRIVATE internal/CurlHttps.cpp
      PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" FILES Https.cppm)
  target_link_libraries(vortex_https PUBLIC VortexNet vortex_options PRIVATE CURL::libcurl vortex_warnings)
  vortex_assert_gpu_free(vortex_https)
  ```
- new `engine/tools/https/Https.cppm`: `export module vortex.https; import vortex.net;` and
  `export namespace vortex::https { std::unique_ptr<net::HttpsClient> MakeCurlHttpsClient(); }` (with `#include <memory>`
  in the global fragment).
- new `engine/tools/https/internal/CurlHttps.cpp`: `module;` `#include <curl/curl.h>` and the standard headers,
  `module vortex.https;` `import vortex.net;`.
- change `engine/CMakeLists.txt`: after `vortex_assert_no_network(VortexSim)`:
  ```cmake
  # rendezvous.md: HTTPS (libcurl) belongs to vortex_https, linked only by vortex_server and the game.
  function(vortex_assert_no_https target)
      vortex_collect_link_closure(${target} _closure)
      foreach(_item ${_closure})
          if(_item MATCHES "^(vortex_https|CURL::libcurl|curl|libcurl)$" OR _item MATCHES "libcurl")
              message(FATAL_ERROR "VORTEX: ${target} must not link '${_item}': HTTPS belongs to vortex_https only "
                                  "(docs/design/rendezvous.md \"The engine side\").")
          endif()
      endforeach()
  endfunction()
  foreach(_t VortexCore VortexPhysics VortexSim VortexNet)
      vortex_assert_no_https(${_t})
  endforeach()
  ```
  and `vortex_assert_no_https(VortexNetTests)` after `vortex_assert_gpu_free(VortexNetTests)` in
  `engine/tests/CMakeLists.txt`, `vortex_assert_no_https(vortex_bot)` in `engine/tools/bot/CMakeLists.txt`.
- change `engine/tools/server/CMakeLists.txt`: link `vortex_https` (PRIVATE); game `CMakeLists.txt`:
  `target_link_libraries(Shredborne PRIVATE ... vortex_https)`.

**Steps** (`CurlHttps.cpp`)
1. `std::once_flag g_Init;` `MakeCurlHttpsClient()` calls `std::call_once(g_Init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); })`
   and returns `std::make_unique<CurlHttpsClient>()`. No `curl_global_cleanup` (decision 7).
2. `struct Job { net::HttpsRequestId id; net::HttpsRequest request; std::shared_ptr<std::atomic<bool>> cancel; };`
   `struct Lane { std::mutex mutex; std::condition_variable wake; std::deque<Job> queue; std::thread thread; };`
   The client has `Lane m_Long, m_Short;`, `std::mutex m_Mutex` guarding `std::map<HttpsRequestId, HttpsResponse> m_Results`,
   `std::map<HttpsRequestId, std::shared_ptr<std::atomic<bool>>> m_Cancels`, `HttpsRequestId m_Next = 0`, and
   `std::atomic<bool> m_Stop{false}`. The constructor starts both threads on `Run(lane)`.
3. `Start(r)`: under `m_Mutex`: `id = ++m_Next`, `m_Cancels[id] = make_shared<atomic<bool>>(false)`. Push the job on
   `r.longPoll ? m_Long : m_Short` under its mutex, `notify_one`. Return `id`.
4. `Take(id)`: under `m_Mutex`, move the result out of `m_Results` and erase both maps' entries; else nullopt.
5. `Cancel(id)`: under `m_Mutex`, set the flag if the entry exists.
6. `Run(lane)`: loop: wait until `m_Stop` or the queue is not empty; stop when `m_Stop` and the queue is empty; pop
   the front; `response = job.cancel->load() ? HttpsResponse{0, "", true, "cancelled"} : Perform(job)`; under
   `m_Mutex`, store it in `m_Results` only if `m_Cancels` still has the id.
7. `Perform(job)`: `curl_easy_init` (null -> failed "curl_easy_init failed"); `curl_slist` of `"Name: value"` per
   header, plus `Content-Type: application/json` when the body is not empty; options: `CURLOPT_URL`,
   `CURLOPT_CUSTOMREQUEST` = method for `POST` and `DELETE`, `CURLOPT_POSTFIELDS` / `CURLOPT_POSTFIELDSIZE` when the
   body is not empty (a `POST` with no body: `CURLOPT_POSTFIELDS ""`, size 0), `CURLOPT_HTTPHEADER`,
   `CURLOPT_TIMEOUT_MS` = `timeoutMs`, `CURLOPT_CONNECTTIMEOUT_MS` = min(`timeoutMs`, 5000), `CURLOPT_IPRESOLVE` =
   `CURL_IPRESOLVE_V4`, `CURLOPT_NOPROXY` = `"*"`, `CURLOPT_NOSIGNAL` = 1, `CURLOPT_FOLLOWLOCATION` = 0,
   `CURLOPT_USERAGENT` = `"vortex-rendezvous/1"`, `CURLOPT_WRITEFUNCTION` appending to a `std::string`,
   `CURLOPT_NOPROGRESS` = 0, `CURLOPT_XFERINFOFUNCTION` returning 1 when the job's flag is set (`XFERINFODATA` =
   the flag), `CURLOPT_ERRORBUFFER`. Certificate checks stay at curl's defaults (on). `curl_easy_perform`:
   `CURLE_OK` -> `{status from CURLINFO_RESPONSE_CODE, body}`; `CURLE_ABORTED_BY_CALLBACK` -> failed "cancelled";
   else failed with the error buffer or `curl_easy_strerror`. Free the list, clean up the handle.
8. Destructor: `m_Stop = true`; set every flag in `m_Cancels`; notify both lanes (take each lane's mutex while
   notifying); join both threads. A running transfer stops at its next progress call (curl calls it at least once a
   second).

**Checks**: build (the configure step runs the new checks: it must pass); `tools/ctest.sh`; doc-check. A smoke test
of the client comes with card 5 (the server against local.mjs). Temporarily add `vortex_https` to VortexNet's links,
configure, see the FATAL_ERROR, and revert (say so in the report).

**Must not break**: CLAUDE.md section 4 layering: Net never links curl; `vortex_bot` and `VortexNetTests` neither.
On Windows MSYS2 provides `mingw-w64-clang-x86_64-curl` (Schannel): an owner step (card 9).

**Done when**: [ ] `vortex_https` builds warning-free and links into `vortex_server` and `Shredborne`; [ ] the
configure check fires when broken on purpose.

**Traps**
- `curl_easy_setopt` takes `long` for numbers: pass `1L`, `static_cast<long>(...)`.
- Never call `Take` from inside the worker thread; never hold `m_Mutex` while `curl_easy_perform` runs.
- `CURLOPT_POSTFIELDS` does not copy: the body string must live until `curl_easy_perform` returns (keep it in the job).

## Card 5: `vortex_server --rendezvous`

| | |
|---|---|
| Goal | The owner's home server registers its room, waits for joins and punches toward each joining player, with no change to sessions that do not use it. |
| Depends on | cards 3, 4 |
| Effort | 1 d (e) |
| Touches | `vortex.net` (`:punch`, `RendezvousRun.cpp`), `engine/tools/server/main.cpp`; wire: no |

**Read first** (about 55 KiB): `engine/src/net/internal/Punch.cpp` (`Options`, `Parse`, `ResolveStun`);
`engine/tools/server/main.cpp` lines 180-330 (flags, tokens, `serverUdp`) and the `pass` lambda and the `openEnded`
loop (`grep -n 'auto pass = \|if (openEnded)'`); `engine/src/net/Rendezvous.cppm`.

**Files**: change `engine/src/net/Punch.cppm`, `internal/Punch.cpp` (`NatFlags`, `ParseNatFlags`,
`ResolveStunServers`; `Parse` uses them), `engine/src/net/internal/RendezvousRun.cpp` (`RendezvousHost`),
`engine/tools/server/main.cpp`.

**Steps**
1. `ParseNatFlags`: for `i` in 1..argc-1, `a = argv[i]`; value forms `--stun X` (X = `argv[i+1]`, which must exist and
   not start with `--`) and `--stun=X`. `--stun` -> push; `--candidates` -> `ParseCandidateKinds` else
   `--candidates expects a list of lan, stun, given`; `--public` -> server role: `ParseIpv4Endpoint` else
   `--public expects A.B.C.D:PORT`; client role: `--public is the server's`. Other arguments are skipped. At the end
   an empty `stun` gets `kDefaultStunServers`. `ResolveStunServers` = today's `ResolveStun` renamed. `Parse` (the R0
   tools) calls `ParseNatFlags` first and keeps its own flags and its refusal of port 27015.
2. `RendezvousHost` constructor: `m_Stun(settings.stunServers, settings.seed * 2 + 1)`,
   `m_Rendezvous(https, settings.rendezvous)`, `m_Traversal(settings.rendezvous.token, <interfaces passing IsLanInterface>, TraversalLimits{}, settings.seed * 2 + 3)`,
   `m_Start = now`. Log `Rendezvous: <url>, room <8 hex>, <n> STUN server(s)`.
3. `RendezvousHost::Update(now, udp)`:
   1. If closing (`m_ClosedAt >= 0`): `m_Rendezvous.Update(now, m_Candidates, m_Stun.Class())`, print its log, return.
   2. For each `d` of `udp.TakeRaw()`: `m_Stun.OnDatagram(d, now)` or else `m_Traversal.OnDatagram(d, now)`.
   3. `m_Out.clear()`; `m_Stun.Update(now, m_Out)`.
   4. Not gathered and (`m_Stun.Settled()` or `now - m_Start >= 7.0`): gathered; log
      `NAT: outside <A:P | ->, class <c>`. `m_Stun.TakeChange()` -> log `NAT: the outside address changed to <A:P>`.
   5. Gathered: `m_Candidates = ChooseCandidates(PunchRole::Server, interfaces, port, kinds, given, m_Stun.Mapped())`;
      `m_Rendezvous.Update(now, m_Candidates, m_Stun.Class())`.
   6. For each join of `TakeJoins()`: `AddJoin(join, now)`; `Added` -> log
      `Punch: join <id> from the Worker, <n> candidate(s)<", unverified" when !verified>` (n = the count in
      `join.candidates`); else `Punch: join <id> refused: <duplicate nonce | 4 joins at once | bad candidates>`.
   7. `m_Traversal.Update(now, m_Out)`; for each outcome: reached -> `Punch: join <id> reached <A:P> (<kind>) in <ms> ms`
      (`{:.0f}` of seconds * 1000); else `Punch: join <id> failed after <s> s: <the client was heard, but the two networks cannot meet | no datagram from the client arrived>`
      (`{:.0f}`: "10 s").
   8. `udp.SendRaw` each of `m_Out`; print the `Rendezvous` log lines with `Log::Info`.
   `Close(now)`: `m_Rendezvous.Close(now)`, `m_ClosedAt = now`. `Closed(now)`: `m_Rendezvous.State() == Closed` or
   `now - m_ClosedAt >= 2.0` (then log `Rendezvous: the delete did not finish within 2 s` once).
4. `main.cpp`, flag checks (where `--listen` is read, before the preload): if `args.Has("--rendezvous")`:
   - no `--listen` -> `Log::Error("--rendezvous needs --listen PORT")`, return 2;
   - `fileToken` -> `Log::Error("--rendezvous needs --token HEX or --new-token: the session file's token is in the repository")`, return 2;
   - URL = `args.Get("--rendezvous")`, else `net::ReadRendezvousUrlFile(Paths::ExecutableDir(), why)`, else
     `Log::Error("--rendezvous: no URL given and {}", why)`, return 2; `ValidRendezvousUrl` else return 2;
   - `net::ParseNatFlags(argc, argv, net::PunchRole::Server, natFlags, why)` else return 2.
   Update the usage text and the header comment (`--rendezvous [URL]`, `--stun`, `--candidates`, `--public`).
5. `main.cpp`, start: right before `lastWall = wallNow();` that precedes `if (openEnded)`, when rendezvous:
   `https = vortex::https::MakeCurlHttpsClient()`; `RendezvousHostSettings{ {url, session.token, server.BuildHashValue(), net::kProtocolVersion}, net::ResolveStunServers(natFlags.stun), natFlags.kinds, natFlags.given, net::LocalInterfaces(), serverUdp->Port(), <random_device seed> }`;
   `host = std::make_unique<net::RendezvousHost>(*https, settings, wallNow())`. `import vortex.https;` at the top.
6. `main.cpp`, `pass`: in the `enet` branch right after `server.Advance(passSeconds);` add
   `if (host) host->Update(wallNow(), *serverUdp);`.
7. `main.cpp`, exit: right after the `openEnded` `while` loop ends (Ctrl-C, `--seconds` or empty): if `host`:
   `host->Close(wallNow())`, then `while (!host->Closed(wallNow())) { host->Update(wallNow(), *serverUdp); sleep 10 ms; }`,
   then `host.reset(); https.reset();` (in that order).

**Checks**
1. build; `tools/ctest.sh`; doc-check PASS.
2. The manual run (local.mjs on TCP 27066, the server on UDP 27065):
   ```
   node engine/tools/rendezvous/local.mjs --port 27066 > build/r1-local.log 2>&1 & L=$!
   T=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
   build/engine/tools/server/vortex_server --session=engine/tools/server/sessions/playground.session --listen 27065 \
       --bind 127.0.0.1 --token $T --seconds 40 --rendezvous http://127.0.0.1:27066 --stun 127.0.0.1:27067 \
       --candidates given --public 127.0.0.1:27065 > build/r1-server.log 2>&1 & S=$!
   sleep 20   # the preload and the register
   ROOM=$(python3 -I -c "import hashlib,sys;print(hashlib.sha256(b'vortex-room-v1'+bytes.fromhex(sys.argv[1])).hexdigest()[:32])" $T)
   curl -s -X POST http://127.0.0.1:27066/v1/join -d "{\"room\":\"$ROOM\",\"cands\":\"-\",\"nonce\":\"00000000000000aa\",\"build\":\"$(grep -o 'build [0-9a-f]\{16\}' build/r1-server.log | head -1 | cut -d' ' -f2)\",\"protocol\":10}"
   wait $S; kill $L
   ```
   The build hex for the join: take it from the curl 409 reply if the grep finds nothing (`error another-build X`),
   then post again with X. Expected in `r1-server.log`: `NAT: outside -, class unknown`,
   `Rendezvous: room <8 hex> registered, 1 candidates`, `Punch: join 1 from the Worker, 0 candidate(s)`,
   `Punch: join 1 failed after 10 s: no datagram from the client arrived`, and at the end
   `Rendezvous: room <8 hex> deleted`; exit 0.
3. `VERIFY_SLOT=1 tools/verify.sh quick` - PASS (vortex_server changed).

**Must not break**: G11-G15 (no `--rendezvous`: no `RendezvousHost`, the pass is unchanged); never port 27015; the
server's tick never waits on HTTPS (only `Take` polls). L14: the live server is never touched.

**Done when**: [ ] the manual run printed the expected lines; [ ] `verify quick` PASS; [ ] the R0 tools still parse
their flags (`--nat-probe --stun 127.0.0.1:27067` exits 0).

**Traps**: `TakeRaw` must be called every pass, or the transport keeps 256 datagrams and drops the rest
(`rawDropped`); the server's `Listen` host takes punches from anyone, which is fine: only proven ones are answered.

## Card 6: `Shredborne --join TOKEN`

| | |
|---|---|
| Goal | A tester types `Shredborne --join <token>` (with `rendezvous.url` beside the game) and plays on the owner's server, with no address and no port. |
| Depends on | card 5 |
| Effort | 1 d (e) |
| Touches | `vortex.net` (`RendezvousRun.cpp`), the game's `src/Session.cpp`, `src/main.cpp`; wire: no |

**Read first** (about 45 KiB): the game's `src/Session.cpp` `RunSession` (whole function, about 130 lines);
`src/main.cpp` (`grep -n 'RunSession\|--connect'`); rendezvous.md "Flow" ("Timeouts", "Choosing"); decisions 9, 12.

**Files**: change `engine/src/net/internal/RendezvousRun.cpp` (`JoinThroughRendezvous`), `src/Session.cpp`,
`src/main.cpp`, `README.md` ("Controls" is unchanged; add `--join` where `--connect` is described, if it is).

**Steps**
1. `JoinThroughRendezvous(https, udp, settings, error)`; a local `now()` (steady clock seconds since the call); a
   `pump` lambda that polls `udp` (`TransportEvent e; while (udp.Poll(e)) {}`), hands `udp.TakeRaw()` to the STUN
   probe first and else to the traversal (when one exists), runs `stun.Update`, the traversal's `Update` (when one
   exists), sends every datagram with `udp.SendRaw`, `udp.Flush()`, and sleeps 5 ms.
   1. `StunProbe stun(settings.stunServers, settings.seed * 2 + 1)`; pump until `stun.Settled()` or 7 s; log
      `NAT: outside <A:P | ->, class <c>`.
   2. `candidates = ChooseCandidates(PunchRole::Client, interfaces, port, kinds, std::nullopt, stun.Mapped())`.
   3. `Rng rng(settings.seed * 2 + 2)`; `first = now()`; `attempts = 0`.
   4. Loop: `++attempts`; `nonce` = two `NextU32` as one `uint64_t`, again while 0. `RendezvousJoin join(...)`; pump
      and `join.Update(now())`, printing `join.TakeLog()`, until `Done()`.
      - Failure `TooManyJoins` and `now() - first + 15 < 60` and not waited before: pump 15 s, continue.
      - Any other failure: `error = ToString(failure)`; return nullopt.
   5. Log `Rendezvous: join <id> posted; the server was last seen <age> s ago, <n> candidate(s)`. Make
      `ClientTraversal traversal(*join.Result(), TraversalLimits{}, now())`; pump until its state is not `Punching`.
   6. Reached: log `Punch: join <id> reached <A:P> (<kind>) in <ms> ms` (`ReachedAfter() * 1000`, `{:.0f}`); pump
      1.0 s more (decision 12); return `Chosen().at`.
   7. Failed: log `Punch: join <id> failed after <s> s: <FailureText(failure, age)>` (`{:.0f}`). If `ServerStale` and
      `attempts < 4` and `now() - first + 5 < 60`: pump 5 s (the traversal gone), continue. Else
      `error = FailureText(...)`, return nullopt.
2. `Session.cpp`: `int RunSession(const CommandLine& args, EngineConfig config, int argc, const char* const* argv)`
   (and the declaration and call in `main.cpp`, where the condition becomes `args.Has("--connect") || args.Has("--join")`).
   - `--join` with `--connect` -> error, return 2. With `--join`: the token is `--join`'s value (`ParseToken`, else
     `--join expects the 32 hex digits of the invite`, return 2); no `--connect` parsing.
   - After the offline-flag check: with `--join`: URL from `--rendezvous` or `ReadRendezvousUrlFile(Paths::ExecutableDir(), why)`
     (else `Log::Error("--join: no rendezvous: {} (or give --rendezvous URL)", why)`, return 2), `ValidRendezvousUrl`;
     `ParseNatFlags(argc, argv, PunchRole::Client, flags, why)` else return 2; `transport = EnetTransport::Open(args.GetString("--bind", ""), uint16_t(args.GetInt("--port", 0)), {}, &error)`
     (null -> return 1); the build hash as `net::Client` makes it (`voxel::MaterialLibrary scratch;`
     `voxel::RegisterDefaultWorldMaterials(scratch);` `net::BuildHash(scratch)`); `JoinSettings`; `auto https = vortex::https::MakeCurlHttpsClient();`
     `chosen = JoinThroughRendezvous(...)`; none -> `Log::Error("Session: {}", error)`, return 1;
     `transport->ConnectTo(*chosen, &error)` else return 1; `server = FormatEndpoint(*chosen)`, `port = chosen->port`;
     `https.reset()`.
   - Without `--join`: today's `EnetTransport::Connect` path, unchanged.
   - Everything from `net::Client client(cs, *transport);` on is unchanged.
3. `main.cpp` header comment: add `Shredborne --join TOKEN [--rendezvous URL] [--stun HOST:PORT] [--candidates KINDS]`.

**Checks**
1. build; `tools/ctest.sh`; doc-check PASS.
2. The manual run, loopback, no NAT: local.mjs on 27066 as in card 5; the server as in card 5 but with
   `--exit-when-empty --seconds 120` instead of `--seconds 40`; after `Rendezvous: ... registered` in its log:
   `build/Shredborne --headless --validation --join $T --rendezvous http://127.0.0.1:27066 --stun 127.0.0.1:27067 --candidates stun --bind 127.0.0.1 --frames 400 > build/r1-client.log 2>&1; echo $?`.
   Expected: exit 0; client: `Punch: join 1 reached 127.0.0.1:27065 (given) in N ms`, then the usual
   `Session: slot 0 applied opSeq ...; world hash H ...` line; server: `Punch: join 1 reached 127.0.0.1:<client port> (peer-reflexive) in N ms`,
   its "client left" line with the same hash H, then `Rendezvous: room ... deleted`.
3. `VERIFY_SLOT=1 tools/verify.sh quick` PASS, and `VERIFY_SLOT=1 tools/verify.sh full --only G15,G16` PASS
   (`--connect` unchanged).

**Must not break**: `--connect` (G15, G16, G17, G19 use it); the movement rule and the replication contracts are
untouched (the session after the punch is today's session); the keep-alive thread still starts only after the
transport is handed to `net::Client`.

**Done when**: [ ] the manual run gave exit 0 and equal hashes; [ ] `--join` with `--connect` exits 2; [ ] gates pass.

**Traps**: never construct `net::Client` before `ConnectTo` (the Client must see the Connected event); never `Poll`
the transport after `ConnectTo` outside `net::Client` (it would eat that event).

## Card 7: `vortex_natbox`

| | |
|---|---|
| Goal | NATs of every kind on one Linux machine, so the real programs, the real ENet path and the real Worker are tested through them (G20). |
| Depends on | `[nat-R0]` (STUN codec); independent of cards 2-6 |
| Effort | 1 d (e) |
| Touches | new tool `engine/tools/natbox/`; wire: no |

**Read first** (about 25 KiB): rendezvous.md "Tests" ("How `vortex_natbox` works", "Three pairs"); decisions 13-15;
`engine/tests/net/NatSim.h` (the same rules in memory); `engine/tools/server/main.cpp` `--parent-pid` handling.

**Files**: new `engine/tools/natbox/CMakeLists.txt`:
```cmake
# vortex_natbox (rendezvous.md "Tests", gate G20): an emulated NAT for one program, and a fake STUN server. Linux only.
if(NOT WIN32)
    add_executable(vortex_natbox main.cpp)
    target_link_libraries(vortex_natbox PRIVATE VortexNet vortex_warnings vortex_cpu_check)
    vortex_assert_gpu_free(vortex_natbox)
endif()
```
and `engine/tools/natbox/main.cpp` (`#include <sys/socket.h> <netinet/in.h> <arpa/inet.h> <poll.h> <unistd.h> <sys/prctl.h> <csignal>`
and the standard headers, then `import vortex.core; import vortex.net;`).

**Command line**
- `vortex_natbox --fake-stun A.B.C.D:PORT [--parent-pid PID]`: answers every STUN Binding request with
  `MakeStunBindingResponse(transaction, source)`. Logs `Natbox: fake STUN on A.B.C.D:PORT`.
- `vortex_natbox --name NAME --outside-ips IP[,IP...] --mapping eim|symmetric --filtering independent|address|port --stun-server A:P [--stun-server A:P] [--parent-pid PID] [--seconds S]`.
  Logs `Natbox: NAME ready: control 127.0.0.1:C, stun alias 127.0.0.1:S[,127.0.0.1:S2]` once its sockets exist, and
  on exit `Natbox: NAME passed N out, M in; dropped K by filtering, J from another inside address`.
- Every run logs the `Build:` line first (`DescribeBuild()`), exits 2 on a bad command line, 0 on SIGTERM / SIGINT,
  on `--seconds`, or when `--parent-pid` is gone (`prctl(PR_SET_PDEATHSIG, SIGTERM)` and `kill(pid, 0)` once a
  second).

**Steps** (the box; `Endpoint` in host order, `htonl` / `htons` at the socket calls)
1. State: `std::optional<Endpoint> inside` (the program, learned from the first datagram to any alias);
   `std::map<Endpoint, int> aliasOf` and `std::map<int, Endpoint> remoteOf` (alias sockets bound to `127.0.0.1:0`);
   `struct Outside { int fd; std::set<Endpoint> sentTo; std::set<uint32_t> sentToIps; };` `std::vector<Outside> outs`;
   EIM: one outside socket (made at the first outbound datagram, bound to `outsideIps[0]:0`); symmetric:
   `std::map<Endpoint, size_t> outFor` (one per remote, bound to `outsideIps[k % n]:0`, k counting the sockets made);
   the control socket on `127.0.0.1:0`. At start, make the alias of each `--stun-server` and print the ready line.
2. Loop: `poll` over the control socket, every alias and every outside socket, 100 ms timeout; read every datagram
   that is ready (`recvfrom`, 2048-byte buffer, `MSG_DONTWAIT` until `EAGAIN`).
3. On an alias `A(R)` from source `src`: if `inside` is empty, `inside = src`; if `src != *inside`, count it and drop.
   Else outbound to `R`: pick the outside socket (EIM: the one; symmetric: `outFor[R]`, made on demand), add `R` to
   its `sentTo` and `R.ip` to `sentToIps`, `sendto(R)`, count `out`.
4. On an outside socket from `O`: filtering `independent` accepts; `address` accepts when `sentToIps` has `O.ip`;
   `port` when `sentTo` has `O`; else count `dropped` and drop. Accepted and `inside` known: `sendto(*inside)` from
   the alias of `O` (made on demand), count `in`.
5. On the control socket: the text `alias A.B.C.D:P` -> make or find the alias of that endpoint, reply
   `alias A.B.C.D:P 127.0.0.1:Q` to the sender; anything else -> `error <what>`.
6. Fake STUN mode: one socket bound to the given address; for each datagram, `ParseStunBindingRequest` true ->
   reply `MakeStunBindingResponse(t, source)` to the source.

**Checks**
1. build; `tools/ctest.sh`; doc-check PASS.
2. Manual: two fake STUN servers `vortex_natbox --fake-stun 127.0.0.9:27067` and `--fake-stun 127.0.0.10:27067`; a box
   `--name s --outside-ips 127.0.0.2 --mapping eim --filtering port --stun-server 127.0.0.9:27067 --stun-server 127.0.0.10:27067`;
   read its two alias ports S1, S2; `vortex_server --nat-probe --bind 127.0.0.1 --listen 27065 --stun 127.0.0.1:S1 --stun 127.0.0.1:S2`
   prints `NAT: outside 127.0.0.2:<p>, class eim`. The same with `--mapping symmetric` prints `class symmetric`
   (two different outside ports). Stop every process.

**Must not break**: nothing outside the new directory; the tool never binds 0.0.0.0 or a port below 1024; ports only
as decision 14 says.

**Done when**: [ ] both manual probes print the expected class; [ ] the exit line counts are plausible (`in` > 0).

**Traps**: an alias socket answers only its own remote; a datagram to an alias from anyone but the program is dropped
(it would otherwise leak around the NAT on loopback). Bind outside sockets to the exact outside IP, never to
127.0.0.1, or the far side sees the wrong source.

## Card 8: Gate G20: three NAT pairs through the real Worker

| | |
|---|---|
| Goal | Every stage from now on proves that an internet join works through emulated NATs: the pairs that should meet meet, by the kinds the design predicts, and the pair that cannot fails cleanly. |
| Depends on | cards 1, 5, 6, 7 |
| Effort | 1 d (e) |
| Touches | `tools/verify.sh`, `tools/lib/verify-gates.sh`, `tools/lib/lock.sh`, `tools/selftest-verify.sh`, fixtures, `CLAUDE.md`, docs/verify.md; wire: no |

**Read first** (about 60 KiB): `tools/verify.sh` (the header comment, `gate_tiers`, `gate_cmd`, the port block,
`gate_G15`), `tools/lib/verify-gates.sh` (header, `judge_G15`), `tools/selftest-verify.sh`, `tools/lib/lock.sh`
`vortex_copy_tree`, docs/verify.md "tools/verify.sh" and "Parallel work", CLAUDE.md section 2's table.

**The pairs** (server box outside 127.0.0.2, client box 127.0.0.3; both programs `--candidates stun,given`)

| Pair | Server box | Client box | Expected |
|---|---|---|---|
| a | eim, independent | symmetric, port | client exit 0, `Punch: join N reached ... (stun)`; server `Punch: join N reached ... (peer-reflexive)`; G15's hash rule |
| b | eim, port | eim, port | client exit 0; both `reached ... (stun)`; G15's hash rule |
| c | eim, port | symmetric, port | client exit 1 with `Punch: join N failed after 10 s`; server `Punch: join N failed after 10 s`; no `Session: slot` line |

Why: in a, the client's probe leaves its box from a new port b' (symmetric); the server's box lets it in (independent
filtering), so the server learns b' (peer-reflexive) and acks there; the client's Ack comes from the server's `stun`
address. In c the server's box drops everything from b' (port filtering), and the client's box drops the server's
probes to b. These are rows 1-3 of rendezvous.md "Who can meet".

**Steps**
1. `tools/lib/lock.sh` `vortex_copy_tree`: add `engine/tools/natbox/vortex_natbox` to the copied binaries.
2. `tools/verify.sh`: `FULL_GATES="$QUICK_GATES G13 G14a G14b G15 G20 G16 G18 G17"`; `gate_tiers`: G20 with the full
   ones; `gate_cmd` G20: `"three vortex_natbox pairs: node local.mjs --port P20, $S/playground.session --listen P20 --rendezvous ..., $G --join T --rendezvous ... --frames 750"`;
   ports: main checkout `P20=27048 P20S=27049`, slot `P20=$((B + 8)) P20S=$((B + 9))`; header comment updated.
   `NATBOX=$V/build/engine/tools/natbox/vortex_natbox`.
3. `gate_G20`: `G_LOG=$V/g20.log`; `need_port G20 "$P20"` and `"$P20S"`; for each pair a, b, c `g20_pair`; the gate
   passes only if all three do. `g20_pair P`: directory `$V/g20-$P`; a fresh token (as G15); `run_start` the fake
   STUN (`--fake-stun 127.0.0.9:$P20S`), the two boxes (`--name server|client`, the pair's flags,
   `--stun-server 127.0.0.9:$P20S`, `--parent-pid "$BASHPID"`); wait up to 10 s for both `Natbox: ... ready` lines and
   read C and S from them with `sed -nE 's/.*control 127\.0\.0\.1:([0-9]+), stun alias 127\.0\.0\.1:([0-9]+).*/\1 \2/p'`;
   `run_start` `node "$ROOT/engine/tools/rendezvous/local.mjs" --port $P20 --caller-ip 127.0.0.3 --server-box 127.0.0.1:$SC --client-box 127.0.0.1:$CC --wait-steps 20 --wait-step-ms 250 --parent-pid "$BASHPID"`,
   wait for its `listening` line; `run_start` the server
   (`--session=$SESSIONS/playground.session --listen $P20 --bind 127.0.0.1 --token $T --exit-when-empty --seconds 300 --rendezvous http://127.0.0.1:$P20 --stun 127.0.0.1:$SS --candidates stun,given --parent-pid "$BASHPID"`),
   wait up to 180 s for `Rendezvous: room .* registered`; `game_run g20-$P-client 300 --join $T --rendezvous http://127.0.0.1:$P20 --stun 127.0.0.1:$CS --candidates stun,given --frames 750`;
   then: pair c -> `kill_gate TERM` the server; else `wait_limit` the server 330 s; stop the boxes, the fake STUN and
   node with `kill_gate TERM`; `judge_G20 $P <server log> <server exit> <client log> <client exit>`.
4. `tools/lib/verify-gates.sh`: give `judge_G15` an optional 5th argument, the gate name (default `G15`), used in its
   `vg_begin`. `judge_G20 PAIR SLOG SEXIT CLOG CEXIT`:
   - a, b: `judge_G15 "$SLOG" "$SEXIT" "$CLOG" "$CEXIT" G20` must pass; then on the client log
     `need_line "Punch: join [0-9]+ reached [0-9.]+:[0-9]+ \(stun\)" "the client reached the server's stun address"`;
     on the server log (a) `need_line "Punch: join [0-9]+ reached [0-9.]+:[0-9]+ \(peer-reflexive\)" "..."` or
     (b) `... \(stun\)`.
   - c: `vg_begin G20 "$CLOG" "$CEXIT"`, `need_exit 1`, `need_build`,
     `need_line "Punch: join [0-9]+ failed after 10 s" "the client's failure"`,
     `forbid_line "Session: slot [0-9]+ applied" "a session after a failed punch"`; on the server log
     `need_line "Punch: join [0-9]+ failed after 10 s" "the server's failure"`.
   Failure messages name the pair (`pair a: ...`).
5. Fixtures: run `VERIFY_SLOT=1 tools/verify.sh full --only G20 --keep-bins`, and copy trimmed logs (keep the
   `Build:` line, the `NAT:`, `Rendezvous:`, `Punch:` and `Session:` lines and the server's "client left" line) to
   `tools/tests/fixtures/verify/G20a-server.log`, `G20a-client.log`, `G20b-...`, `G20c-...`.
6. `tools/selftest-verify.sh`: in `judge()`, a `G20?)` case calling `judge_G20 <pair> $dir/G20<pair>-server.log $code $dir/G20<pair>-client.log ${CLIENT_EXIT:-0}`
   (pair c with `CLIENT_EXIT=1`); cases: `G20a passes`, `G20b passes`, `G20c passes`; bad:
   `G20a: no peer-reflexive reach` (sed `s/\(peer-reflexive\)/(stun)/` on `G20a-server.log`), `G20c: the client met
   the server` (sed the `failed after 10 s` line away on `G20c-client.log`), `G20b: a hash mismatch` (as G15's case).
7. CLAUDE.md section 2: a G20 row after G19, in the table's form:
   `| G20 | three vortex_natbox pairs with node engine/tools/rendezvous/local.mjs (tools/verify.sh gate_G20; about 2 min; P20 27048, P20S 27049) | pair a: the server reaches peer-reflexive, the client stun, hashes equal; pair b: both stun, hashes equal; pair c: both log Punch: join N failed after 10 s, the client exits 1 |`;
   the "Ports" bullet: "G20 27048 / 27049 (slot: B+8 / B+9)". docs/verify.md "tools/verify.sh": the tier table's
   `full` row gets G20, the ports paragraph gets P20 / P20S, and a short G20 paragraph (the pairs table above).

**Checks**
1. `bash -n tools/verify.sh tools/lib/verify-gates.sh tools/selftest-verify.sh`; `tools/ctest.sh -R 'Tools\.'` (the
   self-tests and doc-check) pass.
2. `VERIFY_SLOT=1 tools/verify.sh full --only G20` PASS (all three pairs); then `VERIFY_SLOT=1 tools/verify.sh full`
   PASS.

**Must not break**: the other gates' judges and fixtures (rule 22 of the guide); doc-check's comparison of CLAUDE.md's
table with `tools/verify.sh --list`; ports of other worktrees (only B+8 / B+9 of your slot).

**Done when**: [ ] G20 passes in `full`; [ ] each planted fault fails it with its message; [ ] CLAUDE.md and
docs/verify.md list it.

**Traps**: the boxes must be up before local.mjs (it needs their control ports) and local.mjs before the server; a
pair's processes must all be stopped before the next pair starts (the fake STUN port is reused); the client's
`--frames 750` run takes about 30 s, the server's preload about 10 s.

## Card 9: Close [nat-R1]

| | |
|---|---|
| Goal | The owner can deploy the Worker and play one internet session from the documents alone; the stage is recorded as built. |
| Depends on | cards 1-8 |
| Effort | 0.5 d (e) |
| Touches | documents; wire: no |

**Steps**
1. **docs/owner-checks.md**: check 17 in "Internet sessions": the owner steps of rendezvous.md "Owner steps" (account,
   `wrangler login`, `wrangler d1 create vortex-rendezvous` and the id into `engine/tools/rendezvous/wrangler.toml`,
   `wrangler d1 execute vortex-rendezvous --remote --file schema.sql`, `wrangler deploy`, the URL into
   `rendezvous.url` next to both programs; Windows: `pacman -S mingw-w64-clang-x86_64-curl` in MSYS2 CLANG64), then
   one internet session: the home server `vortex_server --session=... --listen 27015 --token <kept token> --rendezvous`
   (the owner's own server and port), a tester on another network with `Shredborne.exe --join <token>`. Report: the
   tester's `Punch: join N reached ... in N ms` line, the time from start to the world, the F3 round trip, the
   server's `Rendezvous:` and `Punch:` lines. The Windows row: "17 | the Windows client is the joining side".
2. **rendezvous.md**: Status "R0 and R1 built (not reviewed) ..."; the `[nat-R1]` row: "Built: engine ..., game ...";
   "Owner steps" unchanged unless a command differs from what you ran locally.
3. **engine/docs/notes/net.md** section 1: bullets for `Https.cppm` (`HttpsClient`), `Rendezvous.cppm`
   (`Rendezvous`, `RendezvousJoin`, `RendezvousHost`, `JoinThroughRendezvous`), `engine/tools/https` (`vortex_https`),
   `engine/tools/natbox`, `engine/tools/rendezvous`; section 3 (Traps): "HTTPS never runs on a tick or a frame: Net
   holds only the abstract `HttpsClient`; `vortex_https` runs each request on one of two threads (the long poll alone),
   and `Rendezvous` only starts and takes (`vortex_assert_no_https` keeps curl out of Net)" and "The Worker's replies
   are text lines; a route field exists only from local.mjs (G20): never trust it from the internet."
4. **ROADMAP**: `[nat-R1]` built in "Now" (what the tester gets: `--join TOKEN`), the owner's deploy and internet
   session as owner items; "Next" renumbered; the test count.
5. `VERIFY_SLOT=1 tools/verify.sh full --ci` PASS; `VERIFY_SLOT=1 tools/verify.sh soak` in the background, PASS.
6. `grep -rn 'DISABLED_' engine/tests tests | grep -i rendezvous` prints nothing.

**Checks**: doc-check PASS; verify full --ci PASS; soak PASS.

**Done when**: [ ] owner check 17 written; [ ] documents updated; [ ] gates pass; [ ] the report lists the owner's
steps and says **Built**, not reviewed.

**Traps**: never write a real Worker URL, a D1 id or a token into a committed file; `rendezvous.url` stays
uncommitted (add it to `.gitignore` of the game if it is not ignored).

## Values this stage lands

- [LLM guide](README.md) "Values planned across the packs": gate **G20** (tier `full`, ports 27048 / 27049, slot
  B+8 / B+9). No protocol, button, event, message, entity kind, material or `buildHash` input.
- notes/net.md section 2 (the wire registry) does not change: the Worker's API is HTTP, versioned by `/v1/`.
- Tests: +7 GoogleTest cases (`Rendezvous.*`) and `Tools.RendezvousWorker`.
