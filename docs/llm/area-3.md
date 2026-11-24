# [area-3] The server-held profile: task pack

[[../README|docs index]] · [[README|LLM guide]] · [[../design/event-areas|event areas]]

| | |
|---|---|
| Stage | `[area-3]`: a player has an identity key and a profile held by the home server. A siege session proves the identity, keeps what the fight earned (xp, wins, kills), takes the local world's earned progress as a checked claim, and shows each player's level on the roster. A player without a valid proof plays as a guest and earns nothing |
| Design | [event-areas.md](../design/event-areas.md) "Decision" 16-20, "Wire" (protocol 16 rows), "Open questions" 10-12 |
| Code read at | engine `9474e9c`, game `9729e09`, plus `[area-1]` and `[area-2]` (land them first; find code with `grep -n`) |
| Depends on | `[area-2]` (`Hello::playerId`, `GameMode::Siege`, the event's end, `vortex_server --area`, G26) |
| Cards | 6: wire + skeleton (0.5 d), identity and MACs (0.5 d), the profile store and the plausibility rule (1 d), the server's profile path (1.5 d), the game: key, local progress, HUD (1 d), close (0.5 d) |
| Effort | 5 d (e) |
| Changes | wire: **yes**, `net::kProtocolVersion` -> **16** (card 1 only; after `[area-2]`'s 15). `MessageType` 26 `ProfileChallenge`, 27 `ProfileProof`, 28 `ProgressReport`, 29 `ProfileState`; `Roster` entry + `level u8`. Movement rule: no. `buildHash`: no new input. Gate: **none** (unit and session tests; G26 logs the `Profile:` lines but its judge does not read them). No new dependency (SHA-256 and HMAC-SHA256 exist in `vortex.net:crypto`) |

## Before you start

- `git log --oneline | grep -E '\[(area-1|area-2)\]'` shows both in the game root and in `engine/`. Missing: stop.
- The code this stage builds on (check each with `grep -n`):
  - `engine/src/net/Crypto.cppm`: `Sha256` (`Update`, `Finish`, `Of`), `Sha256Digest` (32 B),
    `HmacSha256(key, parts)`, `HmacSha256(key, message)`.
  - `engine/src/net/Messages.cppm`: `PlayerId` (16 B, `[area-2]`), `Hello::playerId`, `Roster`, `MessageType` (25
    `ObjectiveUpdate` the last).
  - `engine/src/net/internal/ServerFight.cpp`: `FightJoin`, `FightLeave`, `DealDamage` (kill credit),
    `StepRoundAndLives` (the event's end), `MakeRoster`; `ServerImpl.cppm`: `playerIds`, `Siege()`, `sessionOver`.
  - `engine/src/net/Session.cppm` `SessionConfig`; `engine/src/net/Server.cppm` `ServerSettings`.
  - `engine/tools/server/main.cpp`: `--area`, `--events`, the in-process bots, the `Fight:` lines.
  - Game: `src/Areas.cpp` (the join, the Hello's id from `player.id`), `src/FightHud.cpp` (roster, banner),
    `engine/src/core/Settings.cppm` `DefaultSettingsPath`.

## Decisions

Settled by us (2026-11-24; the owner may veto).

1. **The key**: 32 bytes, made once by the game with `std::random_device` (eight 32-bit draws; not a rule: nothing
   replicated depends on it), stored as 64 hex digits in `<settings folder>/profile.key` (`DefaultSettingsPath()`'s
   folder), created with mode 0600 on Linux (`std::filesystem::permissions(owner_read | owner_write)`). Missing or
   malformed: a new key (log `Profile: new identity P<id8>`; an existing malformed file is renamed
   `profile.key.bad` first, never deleted). `[area-2]`'s `player.id` is no longer read (log once if it exists).
2. **Derived values**: `playerId` = first 16 bytes of `SHA-256("vortex-id-v1" || key)`; `authKey` =
   `SHA-256("vortex-auth-v1" || key)` (32 B). The label strings are ASCII without a terminator.
3. **Proof**: `HMAC-SHA256(authKey, "vortex-profile-v1" || nonce[16] || playerId[16])`.
   **Report MAC**: `HMAC-SHA256(authKey, "vortex-progress-v1" || playerId || seq u32 || localSeconds u32 || xp u32)`,
   integers little-endian (the wire's byte order: check `ByteWriter::U32`).
4. **Registration is trust on first use**: an unknown id with the `register` flag and its `authKey` is stored if the
   proof verifies under that key. A known id never changes its `authKey` (a register flag on a known id is checked
   against the stored key; the sent key is ignored).
5. **Guest**: no `ProfileProof` within 600 ticks of the challenge, or a wrong one: the player is a guest for the rest
   of its session (`ProfileState{guest}`; log `Profile: slot S is a guest (<timeout|bad proof|unknown id>)`).
   Nothing is saved for a guest. A guest is never kicked.
6. **The store**: one text file per id, `<dir>/<32 hex>.profile`, `key = value` lines (below), written as
   `<name>.tmp` then `std::filesystem::rename` (atomic on one filesystem). `<dir>/store.lock` held with `flock`
   (`LOCK_EX | LOCK_NB`) for the server's lifetime on Linux; a second server on the folder fails to start
   (`Profiles: DIR is used by another server`, exit 1). On other systems: no lock, a warning. Without
   `--profiles DIR` the store is in memory (tests, gates): `Profiles: in memory (nothing kept)`.
7. **File format** (`ProfileStore::Format` / `Parse`, unknown keys refused, the first line a comment):

```
# vortex profile v1
id = 0123456789abcdef0123456789abcdef
auth = <64 hex>
xp = 1234
wins = 3
kills = 40
deaths = 31
unlocks = 0
last_seq = 17
last_report = 1795000000
clamped = 0
over_twice = 0
suspect = 0
```

8. **Server-earned progress** (siege sessions only, verified players only): 10 xp per credited kill, 100 per won
   event (every member of the winning side present at the end), 20 for 7200 ticks (120 s) or more in `Live` during
   the event. `kills`, `deaths`, `wins` counted alike. Saved at the player's leave and at the event's end (whichever
   first; once per player per event).
9. **Plausibility of a claim** (`ProgressReport`), with `now` the server's wall clock in Unix seconds
   (`ServerSettings::wallClock`, injected in tests):
   1. `seq <= last_seq`: ignored (`duplicate`), the state is still sent.
   2. `suspect`: ignored (`suspect`).
   3. `window = last_report == 0 ? 14400 : min(14400, now - last_report + 60)` (int64, not below 0);
      `secs = min(localSeconds, window)`.
   4. `cap = secs / 6` (600 xp an hour, floor). `xp > cap`: accepted `cap`, `clamped += 1`; `xp > 2 * cap`:
      `over_twice += 1`; `over_twice >= 3`: `suspect = 1` (log `Profile: P<id8> marked suspect`).
   5. Accepted: `xp += accepted`, `last_seq = seq`, `last_report = now`; saved at once.
10. **Level** = `1 + ISqrt64(xp / 50)` (`sim::ISqrt64`), at most 255. xp 0 -> 1, 50 -> 2, 200 -> 3, 450 -> 4,
    5000 -> 11.
11. **What the fight reads**: `verified`, `level` (roster), `unlocks` (stored, sent, unused until `[mag-2]`).
    Nothing changes health, damage, charges or spawns.
12. **Local progress** (game): `<settings folder>/progress.txt` with `seq`, `pending_seconds`, `pending_xp`. Every
    60 s of local play (not paused, not inside an area) in which the feet moved at least 1 m: `pending_seconds += 60`,
    `pending_xp += 1`; written at each change. After a `ProfileState{verified}` the client sends
    `ProgressReport{seq + 1, pending_seconds, pending_xp, mac}` once per session; a later `ProfileState` with
    `reportSeq >= seq + 1` sets `seq = seq + 1` and the pendings to 0. A lost answer: the same claim is sent again
    next time with the same seq (the server ignores it as `duplicate` if it took it, and its state then carries the
    seq: the client clears).
13. **Bots** get keys `SHA-256("vortex-bot-key" || seed u64)` (test only) and verify like players; `vortex_bot` and
    `vortex_server`'s in-process bots never report local claims.

## Skeleton

Wire (`engine/src/net/Messages.cppm`, card 1):

```cpp
    inline constexpr uint16_t kProtocolVersion = 16;   // [area-3] profiles (event-areas.md "Wire")

    // MessageType additions:
    //   ProfileChallenge = 26,  ///< server -> client, events channel: prove the player id (siege sessions)
    //   ProfileProof = 27,      ///< client -> server, events channel: the proof (and the auth key on first use)
    //   ProgressReport = 28,    ///< client -> server, events channel: local progress claimed since the last report
    //   ProfileState = 29,      ///< server -> client, events channel: the profile as the server holds it

    using Nonce = std::array<uint8_t, 16>;
    struct ProfileChallenge { Nonce nonce{}; };                       // 17 B
    struct ProfileProof {
        bool registering = false;                                      // flags bit 0; other bits refused
        Sha256Digest mac{};
        Sha256Digest authKey{};                                        // on the wire only when registering
    };                                                                 // 34 or 66 B
    struct ProgressReport { uint32_t seq = 0, localSeconds = 0, xp = 0; Sha256Digest mac{}; };   // 45 B
    struct ProfileState {
        uint8_t flags = 0;              ///< kProfileVerified 1, kProfileGuest 2, kProfileSuspect 4 (others refused)
        uint8_t level = 0;              ///< 0 for a guest
        uint32_t xp = 0;
        uint16_t wins = 0;
        uint64_t unlocks = 0;
        uint32_t reportSeq = 0;         ///< the last accepted ProgressReport::seq
    };                                                                 // 21 B
    // Roster entry: slot, team, kills, deaths, + level u8 (0 = guest or unknown): 7 B each.
```

New partition `vortex.net:profile` (`engine/src/net/Profile.cppm`, `internal/Profile.cpp`; GPU-free, in Vortex::Net):

```cpp
export module vortex.net:profile;

import :crypto;
import :messages;

// docs/design/event-areas.md decisions 16-20. [area-3]
export namespace vortex::net {
    using IdentityKey = std::array<uint8_t, 32>;
    PlayerId PlayerIdOf(const IdentityKey& key);
    Sha256Digest AuthKeyOf(const IdentityKey& key);
    Sha256Digest ProofMac(const Sha256Digest& authKey, const Nonce& nonce, const PlayerId& id);
    Sha256Digest ReportMac(const Sha256Digest& authKey, const PlayerId& id, uint32_t seq, uint32_t localSeconds, uint32_t xp);
    /// @brief Equal in constant time (every byte compared).
    bool SameMac(const Sha256Digest& a, const Sha256Digest& b);

    struct Profile {
        PlayerId id{};
        Sha256Digest auth{};
        uint32_t xp = 0;
        uint16_t wins = 0;
        uint32_t kills = 0, deaths = 0;
        uint64_t unlocks = 0;
        uint32_t lastSeq = 0;
        int64_t lastReport = 0;          ///< Unix seconds of the last accepted claim; 0 = never
        uint32_t clamped = 0, overTwice = 0;
        bool suspect = false;
        friend bool operator==(const Profile&, const Profile&) = default;
    };
    uint8_t LevelOf(uint32_t xp);
    std::string FormatProfile(const Profile& profile);
    bool ParseProfile(std::string_view text, Profile& out, std::string& error);

    /// @brief What a claim did (decision 9).
    enum class ClaimResult : uint8_t { Accepted, Clamped, Duplicate, Suspect };
    struct ClaimOutcome { ClaimResult result = ClaimResult::Duplicate; uint32_t acceptedXp = 0; bool markedSuspect = false; };
    /// @brief Applies a verified ProgressReport to `profile` at wall time `now` (Unix seconds).
    ClaimOutcome ApplyClaim(Profile& profile, const ProgressReport& report, int64_t now);

    /// @brief The profiles of one home server (decisions 6-7). Main thread only.
    class ProfileStore {
    public:
        /// Empty `dir`: in memory. false: the folder is locked by another server or cannot be made (`error`).
        bool Open(const std::filesystem::path& dir, std::string& error);
        std::optional<Profile> Find(const PlayerId& id) const;
        /// Writes the file (tmp + rename) and the cache. false on an I/O error (logged; the cache keeps it).
        bool Save(const Profile& profile);
        size_t Count() const;
        ~ProfileStore();                 // releases the lock
    private:
        // ...
    };
}
```

`ServerSettings` (`engine/src/net/Server.cppm`):

```cpp
        /// [area-3] The profile store (nullptr: profiles off; a siege then treats every player as a guest).
        ProfileStore* profiles = nullptr;
        /// [area-3] Unix seconds now (plausibility windows). Default: std::chrono::system_clock.
        std::function<int64_t()> wallClock;
        /// [area-3] Seed of the challenge nonces; 0 = std::random_device (not a rule: never replicated).
        uint64_t nonceSeed = 0;
```

`ClientSettings` (`engine/src/net/Client.cppm`): `std::optional<IdentityKey> identity;` and
`std::optional<ProgressReport> claim;` (the client fills `seq`, `localSeconds`, `xp`; the MAC is computed inside).
`Client` accessor: `std::optional<ProfileState> Profile() const`.

Tests: `engine/tests/net/ProfileTests.cpp` (suites `ProfileWire`, `Profile`, `ProfileStore`, `Plausibility`,
`ProfileSession`).

## Card 1: Wire 16 and the skeleton

| | |
|---|---|
| Goal | The four messages and the roster's level round-trip and refuse bad bytes; stubs compile; tests written disabled |
| Effort | 0.5 d (e) |
| Touches | net (Messages, new Profile). **Wire: protocol 16 here** |

**Steps**
1. The structs and `MessageType` values of the skeleton; encoders / decoders in `Messages.cpp` in field order.
   `ProfileProof`: `flags` then `mac`, then `authKey` only when bit 0 is set (a set bit with 34 bytes, or a clear bit
   with 66, is refused). `ProfileState`: flags > 7 refused; `level` 0 with the verified flag refused; `level > 0`
   with the guest flag refused.
2. `Roster`: each entry + `level u8` after `deaths` (2 + 7 B each).
3. `kProtocolVersion` = 16; `Profile.cppm` with stub bodies (`// [area-3] card N fills this`), listed in
   `Net.cppm` and `engine/CMakeLists.txt` (`grep -n "Crypto.cppm" engine/CMakeLists.txt`).
4. All tests of cards 2-4 in full, `DISABLED_`.

**Tests** (enabled here)
- `ProfileWire.SizesAreAsDesigned`: challenge 17, proof 34 and 66, report 45, state 21, a 3-entry roster 23 B.
  Count the built encoders' bytes; if they differ, stop and report.
- `ProfileWire.MessagesRoundTripAndRefuseBadBytes`: each message round-trips; `RefusesTrailingAndMissing` (the
  [wp-7] helper pattern); the flag rules of step 1.

**Checks**: build; `tools/ctest.sh -R 'ProfileWire|Wire|FightWire|SiegeWire' --output-on-failure`; `tools/ctest.sh`;
doc-check.

**Docs**: notes/net.md section 2 (protocol 16, the four messages, the roster's size); CLAUDE.md section 5 "Sessions"
(the new message types in the wire list).

**Must not break**: `FightWire.SizesAreAsDesigned` (the roster's entry size changes here: update its expected value
in this card, from 6 to 7 B per entry: this card names the change, so it is not a deviation).

## Card 2: Identity and MACs

| | |
|---|---|
| Goal | Ids, auth keys and MACs as decided, pinned by known values |
| Effort | 0.5 d (e) |
| Touches | net (Profile) |

**Steps**
1. `PlayerIdOf`: `Sha256 h; h.Update("vortex-id-v1"); h.Update(key); first 16 bytes of Finish()`.
2. `AuthKeyOf`: the same with `"vortex-auth-v1"`, all 32 bytes.
3. `ProofMac`: `HmacSha256(authKey, parts {"vortex-profile-v1", nonce, id})` (the parts overload; the string as its
   bytes).
4. `ReportMac`: parts `{"vortex-progress-v1", id, le32(seq), le32(localSeconds), le32(xp)}`.
5. `SameMac`: `uint8_t d = 0; for (i) d |= a[i] ^ b[i]; return d == 0;`.
6. `LevelOf(xp)`: `min(255, 1 + sim::ISqrt64(xp / 50))` (Net imports Sim: allowed by the DAG).

**Tests**
- `Profile.IdsAndKeysAreStable`: key = bytes 0..31; compute `PlayerIdOf` and `AuthKeyOf` once by hand with
  `sha256sum` (`printf 'vortex-id-v1' > f; printf '\x00\x01...\x1f' >> f; sha256sum f`) and pin the hex digits in the
  test (write the command in the test's comment).
- `Profile.TheProofDependsOnEveryInput`: change one bit of the key, the nonce or the id: a different MAC.
- `Profile.LevelsFollowTheSquareRoot`: 0 -> 1, 49 -> 1, 50 -> 2, 200 -> 3, 450 -> 4, 5000 -> 11, 4'000'000 -> 255.
- `Profile.SameMacComparesEveryByte`: equal, first byte differs, last byte differs.

**Checks**: build; `tools/ctest.sh -R 'Profile\.' --output-on-failure`; `tools/ctest.sh`; doc-check.

## Card 3: The profile store and the plausibility rule

| | |
|---|---|
| Goal | Profiles survive a restart of the server; claims are checked as decided |
| Depends on | card 2 |
| Effort | 1 d (e) |
| Touches | net (Profile) |

**Steps**
1. `FormatProfile` / `ParseProfile`: decision 7 (hex lower case, decimal integers, `suspect` 0 / 1). Parse refuses an
   unknown key, a missing key, a duplicate key, bad hex, a value out of its type's range; `error` names the line.
2. `ProfileStore::Open(dir)`: empty -> memory mode. Else `create_directories`; Linux: `open(dir/store.lock,
   O_RDWR | O_CREAT, 0600)` and `flock(fd, LOCK_EX | LOCK_NB)`; failure -> false with `"used by another server"`.
   (`#include <sys/file.h>` and `<fcntl.h>` in the global module fragment, under `#ifdef __linux__`.) Load every
   `*.profile` (name = 32 hex digits + `.profile`; others ignored); a file that does not parse is logged
   (`Profiles: <file> unreadable: <error>`) and skipped, never deleted. Log `Profiles: N loaded from DIR`.
3. `Save`: memory mode -> the cache only. Else write `<hex>.profile.tmp` (truncate), close, check the stream, then
   `rename` over `<hex>.profile`; update the cache.
4. `ApplyClaim`: decision 9 exactly; the caller has checked the MAC. Integer arithmetic in `int64_t`;
   `localSeconds` and `xp` are `uint32_t` (no overflow: `2 * cap` fits in int64).

**Tests**
- `ProfileStore.AProfileSurvivesAReopen`: a temp folder (`std::filesystem::temp_directory_path() /
  "vortex-profile-test-<pid>"`, removed at the end), save two profiles, destroy the store, open again: both equal.
- `ProfileStore.ASecondServerOnTheFolderIsRefused` (Linux only: `#ifdef __linux__`, else `GTEST_SKIP()` with the
  reason "no flock"): two stores on one folder: the second `Open` returns false.
- `ProfileStore.AnUnreadableFileIsSkippedNotDeleted`: write garbage into a `<hex>.profile`: `Open` true, `Count` 0,
  the file still exists.
- `ProfileStore.NoTmpFileIsLeftBehind`: after `Save`, no `*.tmp` in the folder.
- `Plausibility.AFirstClaimMayCoverFourHours`: `lastReport` 0, `localSeconds` 20000, xp 3000: `secs` 14400, cap
  2400: accepted 2400, `Clamped`, `clamped` 1, `overTwice` 0.
- `Plausibility.ClaimsCannotOutrunTheWallClock`: `lastReport = now - 600`, `localSeconds` 3600, xp 100: window 660,
  cap 110: accepted 100 (`Accepted`).
- `Plausibility.ADuplicateSeqIsIgnored`: seq 5 after `lastSeq` 5: `Duplicate`, nothing changes.
- `Plausibility.ThreeGreedyClaimsMarkTheProfileSuspect`: three claims each over twice the cap (seq 1, 2, 3, an hour
  apart): the third sets `suspect` (`markedSuspect`); a fourth honest claim: `Suspect`, nothing added.
- `Plausibility.HonestPlayIsFarBelowTheCap`: an hour of play at 1 xp a minute (3600 s, 60 xp): accepted in full.

**Checks**: build; `tools/ctest.sh -R 'ProfileStore|Plausibility|Profile\.' --output-on-failure`; `tools/ctest.sh`;
doc-check.

**Traps**
- `rename` over an existing file is atomic on Linux; on Windows `std::filesystem::rename` replaces too (C++17), but
  not atomically: acceptable (decision 6 names Linux as the home server).

## Card 4: The server's profile path

| | |
|---|---|
| Goal | A siege challenges, verifies, saves earned progress and checks claims; the roster carries levels |
| Depends on | cards 1-3 |
| Effort | 1.5 d (e) |
| Touches | net (ServerFight, ServerPeers, ServerImpl, Client), tools/server. Wire: as card 1 |

**Steps**
1. `ServerImpl.cppm`, per slot: `Nonce nonce`, `uint32_t challengeTick`, `enum class ProfileStatus : uint8_t {
   None, Waiting, Verified, Guest }`, `std::optional<net::Profile> profile`, `EventEarnings{kills, deaths, xp,
   liveTicks, saved}`; and a nonce generator (`core::Rng` seeded by `nonceSeed`, or `std::random_device` when 0:
   `grep -n "class Rng" engine/src/core/Random.cppm`).
2. `FightJoin` in a siege with `settings.profiles`: make the nonce, send `ProfileChallenge`, status `Waiting`,
   `challengeTick = Tick()`. Without a store: status `Guest` at once (log `Profile: slot S is a guest (profiles off)`).
3. `OnProfileProof` (dispatch it beside the other client messages; refuse it from a peer that is not `Waiting`:
   ignore and log once):
   1. `stored = profiles->Find(playerIds[slot])`.
   2. Known: `key = stored->auth`; else if `registering`: `key = proof.authKey`; else: guest (`unknown id`).
   3. `SameMac(proof.mac, ProofMac(key, nonce, id))` false: guest (`bad proof`).
   4. Unknown and registering: a new `Profile{id, auth = key}`; `Save`; log `Profile: P<id8> registered`.
   5. Status `Verified`, `profile` cached; log `Profile: slot S verified as P<id8>, level L, xp X`; send
      `ProfileState`; mark the roster dirty (levels).
4. Each tick in `StepFight`: `Waiting` for more than 600 ticks -> `Guest` (`timeout`), send `ProfileState{guest}`.
5. `OnProgressReport`: only from a `Verified` slot, once per session (a second one is ignored and logged); MAC
   check with `ReportMac(profile->auth, id, seq, localSeconds, xp)` (bad: ignored, logged); `ApplyClaim(*profile,
   report, wallClock())`; `Save`; log `Profile: P<id8> claim seq N: <accepted|clamped|duplicate|suspect> X of Y xp`;
   send `ProfileState` (with `reportSeq = profile->lastSeq`).
6. Earnings: in `DealDamage`'s kill path, `earnings[killer].kills += 1`, `xp += 10` (credited kills only);
   `earnings[victim].deaths += 1`. Per tick in `Live`: `liveTicks += 1` for joined players. At the event's end
   (before `EventOver`): winners get `xp += 100` and `wins += 1`; each player with `liveTicks >= 7200` gets
   `xp += 20`; then `SaveEarnings(slot)` for every verified slot. `FightLeave` calls `SaveEarnings(slot)` first.
   `SaveEarnings`: once per slot per event (`saved`): `profile->xp += xp` (saturating at `UINT32_MAX`), `kills`,
   `deaths`, `wins`; `Save`; log `Profile: P<id8> earned X xp (kills K, wins W) in event`.
7. `MakeRoster`: `level = Verified ? LevelOf(profile->xp) : 0`.
8. Client: on `ProfileChallenge`, if `settings.identity`: send `ProfileProof{registering = true, mac, authKey}` (the
   client always offers its key; the server ignores it for a known id: decision 4). No identity: nothing (the server
   makes it a guest). On `ProfileState{verified}` the first time and `settings.claim`: send the `ProgressReport`
   with its MAC. Keep the last `ProfileState` (`Client::Profile()`).
9. `vortex_server --profiles DIR` (default: memory) builds one `ProfileStore` for the process (it outlives the
   `--area` events), passed in `ServerSettings::profiles`; `Open` false -> exit 1 with the error. In-process bots get
   identities (decision 13). The `Fight:` line gains `, verified V, guests G`.

**Tests** (`engine/tests/net/ProfileTests.cpp`, the `[area-2]` `SiegeField` with a memory store and a fixed
`wallClock`)
- `ProfileSession.AFirstJoinRegistersAndVerifies`: one client with an identity: `ProfileState` verified, level 1; the
  store holds its id.
- `ProfileSession.ASecondJoinWithAnotherKeyForTheSameIdIsAGuest`: a store with the id and a different auth key; the
  client's proof is made with its own key: guest (`bad proof`); the stored key unchanged.
- `ProfileSession.NoProofMakesAGuestAfter600Ticks`: a client without identity: guest at tick challenge + 601, still
  connected, still playing.
- `ProfileSession.KillsAndAWinAreSaved`: a short siege where bot 0 kills bot 1 twice and its side wins by the
  objective: bot 0's profile xp 10 + 10 + 100 (+ 20 when Live lasted 7200 ticks: make the test's round shorter so it
  does not), kills 2, wins 1; bot 1 deaths 2.
- `ProfileSession.AClaimIsCheckedAndAcknowledged`: a client with a claim `{seq 1, 600 s, 10 xp}`: accepted; its
  `Profile().reportSeq` is 1; the same claim again in a new session: `duplicate`, state `reportSeq` 1.
- `ProfileSession.GuestsEarnNothing`: a guest kills a verified player: nothing saved for the guest; the victim's
  deaths + 1.
- `ProfileSession.TheRosterCarriesLevels`: verified players' levels, a guest's 0.
- `Siege.*`, `Fight.*` unchanged.

**Checks**: build; `tools/ctest.sh -R 'Profile|Siege|Fight\.' --output-on-failure`; `tools/ctest.sh`; doc-check; the
G26 command by hand (exit 0; `Profile: slot S verified` four times per event; the `Fight:` line's `verified 4`).

**Must not break**: G22 (deathmatch sends no challenge: `FightJoin` checks `Siege()`), G26's judged lines.

## Card 5: The game: key, local progress, HUD

| | |
|---|---|
| Goal | The player has a lasting identity, earns local progress, and sees level and xp in the siege |
| Depends on | card 4 |
| Effort | 1 d (e) |
| Touches | game (`src/Areas.cpp`, `src/main.cpp`, `src/FightHud.cpp`, new `src/Profile.cpp/.h`). Wire: no |

**Steps**
1. `src/Profile.cpp`: `LoadOrMakeIdentity(folder) -> net::IdentityKey` (decision 1) and `LocalProgress`
   (`Load`, `Save`, `Tick(seconds, feet, playing)`, `Claim() -> std::optional<net::ProgressReport>`, `Acknowledge(
   reportSeq)`) (decision 12). Files under `DefaultSettingsPath().parent_path()`; with no home folder: no identity
   (guest), no progress, log once.
2. The local game calls `LocalProgress::Tick` each frame (playing = not paused, not in a menu, not in the editor);
   the area client sets `ClientSettings::identity` and `claim`; after the session, `Acknowledge(client.Profile()->
   reportSeq)` when a state arrived.
3. HUD: the roster and scoreboard show `L4` after names (`GUEST` for level 0); after `ProfileState{verified}` a line
   under the banner for 4 s: `PROFILE P1A2B3C4  LEVEL 4  XP 812`; at the event's end `+130 XP` from the difference of
   the first and last `ProfileState` xp (the server sends a state after saving: card 4 step 6 sends one with the
   save). A guest sees `PLAYING AS GUEST - PROGRESS NOT SAVED` for 4 s.
4. Headless: `--area-test` logs `Profile: identity P<id8>, local claim seq N: S s, X xp` and, when the state came,
   `Profile: verified, level L, xp X`.

**Checks**: build; `tools/ctest.sh`; doc-check; `VERIFY_SLOT=1 tools/verify.sh quick`; the G25 pair by hand twice (the
second run: the same id, `duplicate` or a new seq, never a new registration).

**Docs**: `docs/owner-checks.md` **check 25**: "Profile: join the siege, see your level, win or score kills, leave,
join again: the xp is kept; delete nothing; a friend on another machine has their own id; play locally for 10
minutes, join: the claim (10 xp at most) shows in the log". `docs/verify.md`: the `Profile:` lines (not judged).

## Card 6: Close

1. `VERIFY_SLOT=1 tools/verify.sh full --ci`; `VERIFY_SLOT=1 tools/verify.sh soak` in the background; `tools/build.sh
   ci` clean. `grep -rn 'DISABLED_' engine/tests tests` shows no test of this stage.
2. event-areas.md: stage row [area-3] -> **Built**; "Open questions" 10-12 kept with their answers; the profile file
   format as built. The game's live-install notes (the owner's: `tools/install-live.sh` or its docs; read it, do not
   run it): `--profiles ~/vortex-live/profiles` for the area server, one line on backups (open question 12). Never
   install or start the live server.
3. ROADMAP "Now" / "Next" (the area stages done; `[wp-8]` next), the test count; CLAUDE.md section 5 "Sessions":
   `ProfileChallenge`, `ProfileProof`, `ProgressReport`, `ProfileState` and the rule "a profile is the server's;
   progression never changes a fight's numbers" with `ProfileSession.KillsAndAWinAreSaved` and
   `Plausibility.ThreeGreedyClaimsMarkTheProfileSuspect`.

## Values this stage lands

- `net::kProtocolVersion` 16. `MessageType` 26 `ProfileChallenge` (17 B), 27 `ProfileProof` (34 / 66 B), 28
  `ProgressReport` (45 B), 29 `ProfileState` (21 B); `Roster` entries 7 B (+ `level`).
- llm/README "Values planned across the packs": protocol 16 `[area-3]`, `MessageType` 26-29; no gate.
- Owner check 25. No `PlayerButton`, `EventKind`, `WorldOpKind`, `EntityKind`, `RefuseReason`; `kPlayerRuleRevision`
  and `buildHash` inputs unchanged; no new dependency.
