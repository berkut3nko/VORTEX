module;

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

export module vortex.net:bot;

import vortex.voxel;
import vortex.sim;
import :transport;
import :client;
import :drill;
import :entities;
import :server;

export namespace vortex::net {

    /// @brief The game's chunk store (its GPU chunk buffer) and the slots it keeps free for what a
    /// session adds (networking.md stage 7a M-b): a bot with the game's window gets the same.
    inline constexpr uint32_t kGameStoreSlots = 12288;
    inline constexpr uint32_t kGameStoreHeadroom = 2048;

    /// @brief How a Bot plays (docs/design/networking.md "Stage 8": vortex_server's in-process bots and
    /// vortex_bot share it).
    struct BotSettings {
        uint32_t id = 0;                  ///< its number in the logs ("Client N", "bot N")
        uint64_t seed = 0;                ///< the pilot's seed; 0 = 0xb07 + id
        /// Token, presentation, window (windowChunks > 0: the game's store and headroom), generator wrap.
        ClientSettings client;
        PilotSettings pilot;
        bool walkAcross = false;          ///< waypoints from the play bounds' -x edge to +x along the middle z and back (set at the Welcome)
        bool stand = false;               ///< no controls at all
        /// Injected fault: its generator makes this chunk differently (client.wrapGenerator, BreakChunks);
        /// it stands until that chunk was installed from the server's data, then plays.
        bool hasBrokenChunk = false;
        glm::ivec3 brokenChunk{0};
        /// Injected fault: another build's terrain everywhere (client.wrapGenerator): server terrain mode.
        bool brokenGenerator = false;
        unsigned jobThreads = 0;          ///< its simulation's job threads; 0 = 2 with a window, else 1
        uint32_t chunkCapacity = 0;       ///< the session's chunkCapacity (0 = the region's); a window uses kGameStoreSlots
        bool packHidden = true;           ///< the session's packing (with a window)
    };

    /**
     * @brief One bot player (docs/design/networking.md stages 4-8): a net::Client over the owner's
     * transport, the sim::Simulation built from the Welcome (the game's store and packing with a
     * window), a BotPilot, and per pass the client's due ticks with the pilot's controls. GPU-free.
     * @details The owner drives the clock (Pass gets the seconds the pass covers: simulated or wall
     * clock) and the transport below the client (it flushes it after Pass). The owner also records the
     * time each opSeq was applied (RecordApplied after each pass, on its own clock: server ticks in
     * vortex_server, steady-clock seconds in vortex_bot).
     */
    class Bot {
    public:
        Bot(BotSettings settings, ITransport& link);
        ~Bot();
        Bot(const Bot&) = delete;
        Bot& operator=(const Bot&) = delete;

        /// @brief One pass of `seconds`: once Welcomed builds the simulation and attaches it (the region
        /// load calls `whileLoading` between its steps: the owner keeps its transports serviced), then
        /// runs the ticks the client's clock says are due, with the pilot's controls when `acting` (none
        /// while it stands, or while an injected broken chunk is not repaired yet), and shows the casts
        /// (sim::PresentBlast) when presenting.
        void Pass(double seconds, bool acting, const std::function<void()>& whileLoading = {});
        /// @brief One pass on a wall clock (vortex_bot): Pass with the seconds since the last pass
        /// *began* (review of stage 8: the work inside a pass belongs to the time the next one covers,
        /// or the client's clock runs slow and keeps jumping forward). The pass that builds and attaches
        /// the simulation (the region load) counts the next one from its end instead, so a long load does
        /// not come back as a burst of catch-up ticks. Returns `clock()` at the pass start.
        double PassOnClock(const std::function<double()>& clock, bool acting, const std::function<void()>& whileLoading = {});
        /// @brief Records `now` as the time every opSeq applied since the last call was applied (from the
        /// join point on, once Playing).
        void RecordApplied(double now);
        /// @brief (opSeq, time applied) for every event from the join point on, as recorded.
        const std::vector<std::pair<uint32_t, double>>& Applied() const { return m_Applied; }
        /// @brief The time (owner's clock, RecordApplied) the client was first seen Playing; < 0: not yet.
        double PlayingSince() const { return m_PlayingSince; }

        Client& GetClient() { return *m_Client; }
        const Client& GetClient() const { return *m_Client; }
        /// @brief Its simulation: nullptr before the Welcome, or when the attach failed.
        sim::Simulation* GetSimulation() const { return m_Simulation.get(); }
        /// @brief Its pilot: nullptr before the Welcome.
        const BotPilot* Pilot() const { return m_Pilot.get(); }
        const BotSettings& Settings() const { return m_Settings; }
        bool AttachFailed() const { return m_AttachFailed; }
        bool Loading() const { return m_Loading; }
        /// @brief Waiting for its injected broken chunk to be repaired (it stands meanwhile).
        bool WaitingForRepair() const;
        uint32_t CastsShown() const { return m_Casts; }
        float MinX() const { return m_MinX; }
        float MaxX() const { return m_MaxX; }
        /// @brief Wall seconds of its simulation's Initialize and of Attach (the region load).
        double InitializeSeconds() const { return m_InitSeconds; }
        double AttachSeconds() const { return m_AttachSeconds; }
        /// @brief Ticks it was asked to act on once it had first predicted (acting, not standing, not
        /// waiting for a repair), and of them the ticks its pilot drove (it was Predicting): review of
        /// stage 8, so a bot that silently stops predicting fails its verdict (BotGates::minActedShare).
        uint64_t WantedTicks() const { return m_WantedTicks; }
        uint64_t ActedTicks() const { return m_ActedTicks; }
        /// @brief A walker: what was near its feet 1 s into its longest still stretch (held by the
        /// collider gate or not and its held ticks so far, the nearest replicated object, the ground and
        /// head-height materials, on the ground or not); empty when no stretch lasted 1 s. The walk line
        /// carries it (DescribeBotWindow).
        const std::string& LongestStillContext() const { return m_Still.longestContext; }
        /// @brief The same 2 s into its longest confined stretch (BotPilot::LongestConfined, within 4 m).
        const std::string& LongestConfinedContext() const { return m_Confined.longestContext; }

    private:
        /// What was at the capsule some way into a stretch, kept for the longest (NoteStretch).
        struct StretchNote {
            std::string context, longestContext;   ///< of the stretch going on / the longest
            uint32_t start = 0, longestStart = 0;   ///< their BotPilot::StillStretch::startTick
        };
        void BuildAndAttach(const std::function<void()>& whileLoading);
        void NoteStill();
        void NoteStretch(StretchNote& note, const BotPilot::StillStretch& now, const BotPilot::StillStretch& longest, double contextSeconds,
                         double logSeconds, const char* what);

        BotSettings m_Settings;
        ITransport& m_Link;
        std::unique_ptr<Client> m_Client;
        std::unique_ptr<sim::Simulation> m_Simulation;
        std::unique_ptr<BotPilot> m_Pilot;
        bool m_AttachFailed = false;
        bool m_Loading = false;
        bool m_Repaired = false;
        uint32_t m_Casts = 0;
        float m_MinX = 1e30f, m_MaxX = -1e30f;
        double m_InitSeconds = 0.0, m_AttachSeconds = 0.0;
        double m_LastPassAt = -1.0;
        bool m_Predicted = false;
        uint64_t m_WantedTicks = 0, m_ActedTicks = 0;
        StretchNote m_Still, m_Confined;   ///< BotPilot::Still / Confined
        uint32_t m_Seen = 0;
        bool m_Counting = false;
        double m_PlayingSince = -1.0;
        std::vector<std::pair<uint32_t, double>> m_Applied;
    };

    /**
     * @brief The drain rule of a client that has stopped playing (the game's headless exit, vortex_bot):
     * it applied the server's last operation, is Synced, every copy of a server object rests or stands
     * still, and that has held, with no new opSeq from the server, for `holdSeconds`.
     * @details "Stands still" (stage 8, the arena soak): a server body over terrain without a collider is
     * frozen (TerrainColliders::GuardBodies; on a 250 m region the server cooks colliders only near the
     * players and the spawn) and counts as awake there, so its copy never gets an EntityRest; it does not
     * move either. A copy that is not resting must stay within 1 mm of where it was when the hold began.
     */
    class DrainWatch {
    public:
        explicit DrainWatch(double holdSeconds = 1.0) : m_Hold(holdSeconds) {}
        /// @brief Feeds the client's state at `now` (seconds, any clock); true once the rule has held for
        /// the hold time.
        bool Update(const Client& client, double now);
        /// @brief The rule holds at the last Update (not yet for the hold time, maybe).
        bool Holds() const { return m_CaughtUp; }
        /// @brief Copies not resting at the last Update (standing still or moving).
        uint32_t NotResting() const { return m_NotResting; }
        /// @brief What the last Update saw, for a log line when a drain does not settle: "applied A of
        /// B, synced / not synced, N copies not resting (M moved or new), held for S s".
        std::string Describe(double now) const;
        void Reset() {
            m_CaughtUp = false;
            m_Seen = 0;
            m_Since = 0.0;
            m_Still.clear();
        }

    private:
        double m_Hold;
        bool m_CaughtUp = false;
        uint32_t m_Seen = 0;
        double m_Since = 0.0;
        uint32_t m_NotResting = 0, m_Moved = 0;
        uint32_t m_Applied = 0, m_Server = 0;
        bool m_Synced = false, m_Playing = false;
        std::unordered_map<uint32_t, glm::vec3> m_Still;   ///< non-resting copies' positions when the hold began
    };

    /// @brief p50 / p99 / max (nearest rank) of a list of values.
    struct Percentiles {
        double p50 = 0.0, p99 = 0.0, max = 0.0;
        size_t count = 0;
    };
    Percentiles PercentilesOf(std::vector<double> values);

    /// @brief Durations in fixed buckets (recheck of stage 8: vortex_server kept every pass time of a
    /// listening server, about 0.9 MB an hour, and sorted a copy at exit): `bucketSeconds` wide, the last
    /// bucket for everything from `buckets - 1` buckets on. Percentiles are bucket upper bounds (the
    /// largest value for the overflow bucket), so within one bucket of PercentilesOf's.
    class DurationHistogram {
    public:
        explicit DurationHistogram(double bucketSeconds = 0.00005, size_t buckets = 2001) : m_Width(bucketSeconds), m_Counts(buckets, 0) {}
        void Add(double seconds);
        uint64_t Count() const { return m_Count; }
        double Max() const { return m_Max; }
        /// @brief The duration under which `fraction` of the values stayed (the rank PercentilesOf uses);
        /// 0 with no value.
        double Percentile(double fraction) const;
        Percentiles Summary() const;

    private:
        double m_Width;
        std::vector<uint64_t> m_Counts;
        uint64_t m_Count = 0;
        double m_Max = 0.0;
    };

    /// @brief A hash of the resting objects' NetIds and rest poses (origin to 1 mm, rotation to 1e-4,
    /// q and -q alike) and their count: what two processes compare besides EntityListHash (stage 8: the
    /// rest states and poses vortex_server's in-process entity check compared).
    uint64_t EntityRestHash(const std::vector<EntityInfo>& entities);

    /// @brief "Session: slot S applied opSeq A of the server's B; world hash H (N chunks) over [box];
    /// joined at opSeq J, tick T": the game's and vortex_bot's exit line (tools/session-check.sh and
    /// tools/soak.sh parse it; the join point since the recheck of stage 8: a slot is reused after a leave).
    std::string SessionHashLine(const Client& client);
    /// @brief vortex_server's line when the client in `slot` leaves (call it from the SetOnClientLeft
    /// callback): "Session: the client of slot S left; server at opSeq A, tick K, world hash H (N chunks)
    /// [or: world hash not taken (its window moves)]; window hash W over [box]; joined at opSeq J, tick T".
    /// The hashes come from the server's per-chunk cache; the region's hash only when the client's box
    /// is the region's window (a moving window compares its window hash only). tools/soak.sh matches a
    /// client's SessionHashLine to it by slot and join point.
    std::string ServerLeaveLine(Server& server, uint8_t slot);
    /// @brief "Session: slot S entities at opSeq A: N object(s), R resting, id hash X, rest hash Y".
    std::string SessionEntitiesLine(const Client& client);
    /// @brief The server's "Session: entities at opSeq A: ..." line (the same fields).
    std::string ServerEntitiesLine(const Server& server);
    /// @brief The objects that are not resting, for a comparison with a tolerance (review of stage 8: the
    /// rest hash covers resting objects only, and a body frozen awake for good below a window never
    /// rests): "N awake[: id (x, y, z); ...]" sorted by NetId, positions in metres to 1 mm.
    std::string AwakeEntitiesText(const std::vector<EntityInfo>& entities);
    /// @brief "Session: slot S awake at opSeq A: N awake: id (x, y, z); ..." (game, vortex_bot) and the
    /// server's "Session: awake at opSeq A: ..." (logged after its entities line); tools/soak.sh and
    /// tools/session-check.sh compare them with 5 cm of tolerance.
    std::string SessionAwakeLine(const Client& client);
    std::string ServerAwakeLine(const Server& server);

    /// @brief The per-bot lines vortex_server has logged since stages 4-7b, from the bot alone, in the
    /// same text (scripts and docs parse them; Bot.ReportLinesKeepTheirFormat): `Client N movement` (stage
    /// 8: with "final error E m"), `Client N objects`, `Join: bot N`, and `Client N window` (plus `Client
    /// N is not synced` and, for a walker, `Client N walk`). The owner adds its own halves.
    std::string DescribeBotMovement(const Bot& bot, double tickSeconds);
    std::string DescribeBotObjects(const Bot& bot);
    std::string DescribeBotJoin(const Bot& bot, double tickSeconds);
    std::vector<std::string> DescribeBotWindow(const Bot& bot);

    /// @brief The bot-side gates (stage 8, review of the plan).
    struct BotGates {
        uint32_t minHashChecks = 1;       ///< HashChecks compared at least
        uint32_t minEntityChecks = 1;     ///< EntityChecks compared at least (in full or partly)
        float maxFinalError = 0.05f;      ///< the own position's last error after the drain (metres)
        bool storeGate = true;            ///< a window: never full, and kGameStoreHeadroom kept free
        // Review of stage 8: that the bot played as it was told (0 = not judged; vortex_bot sets them).
        /// Bot::ActedTicks() at least this share of Bot::WantedTicks() (it kept predicting).
        float minActedShare = 0.0f;
        /// With casts / crates every N ticks: the pilot's casts / crates at least this share of
        /// ActedTicks / N.
        float minPilotShare = 0.0f;
        /// Casts the server did not run at most this share of the pilot's casts (plus 2).
        float maxRejectedCastShare = 1.0f;
        /// A walker (BotSettings::walkAcross): x from MinX to MaxX at least this share of the waypoints'
        /// span less 2 x PilotSettings::reachMetres (what the pilot can cover: it reaches a waypoint that
        /// near), at least 2 waypoints reached, fewer given up than reached, and (a moving window) window
        /// updates sent.
        float minWalkShare = 0.0f;
        /// A walker: its longest still stretch (BotPilot::LongestStill: within 1 m for this long while it
        /// headed for a waypoint) at most this many seconds; 0 = not judged. A pilot that goes round what
        /// holds it would otherwise hide a walker that snags (G19 of 2026-11-22 stood 150 s).
        float maxStillSeconds = 0.0f;
        /// A walker: its ticks in a detour (BotPilot::DetourTicks) at most this share of the ticks it
        /// headed for a waypoint (BotPilot::WalkTicks); 0 = not judged.
        float maxDetourShare = 0.0f;
        /// A walker: its longest confined stretch (BotPilot::LongestConfined: within 4 m for this long
        /// while it headed for a waypoint) at most this many seconds; 0 = not judged. A walker held in a
        /// pit that shuffles more than 1 m restarts its still stretch, and a 60-90 s episode hides in a
        /// run's detour share.
        float maxConfinedSeconds = 0.0f;
    };
    /// The walker's snag bounds vortex_bot and vortex_server judge (G13, G17, G19). Still: 20 s is 4
    /// waypoints given up in one place; on 2026-11-22 a passing arena soak's and G13's longest stretch was
    /// 1.9 s (the spawn), the failed G19 stood 150 s. Detour share: the two passing soaks with the detour
    /// spent at most 3.8 / 2.9 % stepping aside, 6.6 / 4.9 % with the straight tries between (estimated
    /// from their legs), a later soak 2.3 % and G13 3.7 % (measured); the failed run's 43 give-ups alone
    /// are about 29 %. 15 % is over twice the worst pass and half the failure.
    /// Confined (within 4 m): a walker that walks crosses 4 m in 0.6 s running, about 1 s walking; its
    /// healthy runs' longest confined stretch is the spawn's (KWALKER_CONFINED_MEASURED). 45 s is 9
    /// waypoints given up in one pit, over twice the still bound (a detour's legs aside are 0.5-1.5 s of
    /// running, so a slow way round a wide obstacle stays within 4 m longer than within 1 m), and under
    /// the 60-90 s an episode needs to hide in the detour share. The spawn's collider hold counts in
    /// both stretches: it ends by itself in about 2 s, and one that does not is a fault worth failing.
    inline constexpr float kWalkerMaxStillSeconds = 20.0f;
    inline constexpr float kWalkerMaxDetourShare = 0.15f;
    inline constexpr float kWalkerMaxConfinedSeconds = 45.0f;
    struct BotVerdict {
        bool ok = true;
        std::vector<std::string> reasons; ///< what failed, one short phrase each
        std::string checksNote;           ///< " - its broken chunk ... was found and repaired" and so on (the Checks line's tail)
    };
    /**
     * @brief The bot's own verdict after its drain: attached, Synced, its hash complete, no chunk
     * skipped or unsynced; server terrain mode exactly when its generator was broken; no check out of
     * order; no entity mismatch; hash mismatches only for an injected broken chunk and then exactly that
     * chunk, repaired from data; enough checks compared; the final position error; the store gate.
     * The comparisons with the server are the owner's (vortex_server) or a script's (tools/soak.sh).
     */
    BotVerdict JudgeBot(const Bot& bot, const BotGates& gates);
    /**
     * @brief The walker's snag gates (BotGates::maxStillSeconds, maxDetourShare, maxConfinedSeconds)
     * against its pilot, one phrase per failure ("it stood still ...", "it spent ... detouring", "it
     * stayed within 4 m ..."); JudgeBot adds them for a
     * walker. `tickSeconds`: the session's tick length.
     */
    std::vector<std::string> WalkerSnagFailures(const BotPilot& pilot, const BotGates& gates, double tickSeconds);

    /**
     * @brief What a client's own checks say against it (review of stage 8; the game's headless exit and
     * JudgeBot share it): a check out of its stream position, an entity mismatch, a HashCheck that found
     * a chunk different (none is expected without an injected fault), and server terrain mode unless
     * `terrainModeExpected` (a generator made to differ: --other-terrain, --broken-generator) or not in
     * it when expected. Empty: nothing against it.
     */
    std::vector<std::string> ClientCheckFailures(const Client& client, bool terrainModeExpected);

    /**
     * @brief The broken-chunk candidate of a server (stage 7b's rule: pristine, server-Dense, not a probe,
     * within 12 m of the spawn), stage 8: buried at least `buriedMetres` under the ground of its column,
     * so casts at the surface around the spawn do not reach it before its repair. false (`why`) when
     * there is none.
     */
    bool BrokenChunkCandidate(Server& server, glm::ivec3& out, std::string* why, double buriedMetres = 3.0);
    /// @brief Whether `chunk` may be a broken chunk (the rule above without the burial); `why` when not.
    bool UsableBrokenChunk(Server& server, const glm::ivec3& chunk, std::string* why);

    /// @brief Resident memory of this process in MiB (Linux /proc; 0 elsewhere).
    double ResidentMiB();

    /// @brief One line of vortex_server's --timeline file (stage 8): after the pass that started at
    /// `nanoseconds` (std::chrono::steady_clock, the machine's monotonic clock) the server's last opSeq
    /// was `opSeq` and `joining` clients had join data not yet acknowledged.
    struct TimelineEntry {
        uint32_t opSeq = 0;
        int64_t nanoseconds = 0;
        uint32_t joining = 0;
    };
    /// @brief Parses a timeline ("ops A NS J" lines, '#' comments). A last line without its newline (the
    /// server still writing) is ignored; any other malformed line fails with its line number in `error`.
    bool ParseTimeline(const std::string& text, std::vector<TimelineEntry>& out, std::string* error);

    /// @brief Event latency on one host (stage 8): applied time - made time, in milliseconds.
    struct EventLatency {
        Percentiles all;                  ///< every event made once the bot played
        Percentiles duringJoins;          ///< ... made while some client was joining
        Percentiles quiet;                ///< ... made while no client was joining (review of stage 8: O4's rule compares the two)
        size_t beforePlaying = 0;         ///< events made before the bot played (its own load: apart)
        double beforePlayingMaxMs = 0.0;
        size_t unknown = 0;               ///< applied events the timeline does not reach (not counted)
    };
    /**
     * @brief Matches each applied event (opSeq, steady-clock seconds) with the first timeline entry that
     * reaches its opSeq (the pass that made it). Events made before `playingSince` (seconds, the bot's
     * own join: its load holds them) are counted apart.
     */
    EventLatency LatencyFromTimeline(const std::vector<TimelineEntry>& timeline, const std::vector<std::pair<uint32_t, double>>& applied,
                                     double playingSince);
}
