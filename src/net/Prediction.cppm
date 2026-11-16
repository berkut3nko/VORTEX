module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

export module vortex.net:prediction;

import :messages;

export namespace vortex::net {

    /// @brief The client clock's rules (docs/design/networking.md "Time and ticks").
    struct ClockSettings {
        /// Wanted arrival margin of an input, in ticks, before jitter: "RTT/2 plus a margin of 2 ticks
        /// + measured jitter" (the RTT/2 is in the margin the server measures).
        int32_t baseMargin = 2;
        /// Ticks between two input packets (net::ClientSettings::inputSendInterval). The server
        /// reports the margin of a packet's newest input; the oldest new one in it is
        /// `packetTicks - 1` ticks less early, so that much is added to the target.
        int32_t packetTicks = 1;
        /// The jitter estimate is the mean absolute change of consecutive margins; it adds
        /// ceil(jitterFactor x estimate) ticks to the target.
        float jitterFactor = 2.0f;
        /// Consecutive reports with a margin below 0 that make the clock jump forward at once, by
        /// `target - margin`; also consecutive reports at least `shortBy` ticks short of the target
        /// (review of stage 4: a +50 ms ping step left a 3-tick deficit that the 2 % catch-up took
        /// about 3 s to make up), by what the least short of them misses.
        uint32_t lateReports = 3;
        int32_t shortBy = 2;
        /// Rate of the clock while every margin of the last `surplusWindow` reports is above the target.
        double bleedRate = 0.98;
        /// Rate of the clock while the lowest margin of the last `surplusWindow` reports is at or
        /// above 0 but below the target (stage 4 addition: without it a clock placed a little short
        /// stays short for good, and every lost input packet makes inputs late).
        double catchUpRate = 1.02;
        uint32_t surplusWindow = 16;
        /// A margin above this and above the target for `resyncTicks` server ticks: hard resync (the
        /// clock holds for the surplus, margin - target ticks).
        int32_t resyncAbove = 8;
        uint32_t resyncTicks = 60;
    };

    /// @brief What a margin report asks of the clock.
    struct ClockAction {
        uint32_t jumpTicks = 0;   ///< run this many extra ticks now
        uint32_t holdTicks = 0;   ///< run this many ticks fewer (hard resync)
    };

    /**
     * @brief The client's tick clock: how many ticks to run for the time that passed, kept ahead
     * of the server by just enough that inputs arrive before they are needed.
     * @details Control is asymmetric (networking.md "Time and ticks"): margins below 0, or
     * `shortBy` or more short of the target, in `lateReports` consecutive snapshots jump the clock
     * forward at once by what is missing; a surplus is bled off at 2 % (the clock runs at
     * `bleedRate`) and a smaller deficit is made up at 2 % (`catchUpRate`), both judged on the
     * lowest margin of the last `surplusWindow` reports; a margin above `resyncAbove` and the target
     * for `resyncTicks` holds the clock for the surplus (a server hitch recovers before that). Reports about inputs made
     * before the last jump or hold are ignored (they do not show it yet), and so are repeats about
     * an input already reported (snapshots repeat the last margin while no input arrives). GPU-free
     * and deterministic: tested with margin sequences.
     */
    class TickClock {
    public:
        explicit TickClock(ClockSettings settings = {}) : m_Settings(settings) {}

        /// @brief Ticks to run for `seconds` (at the current rate, plus jumps, minus holds), at most
        /// `maxTicks` (the rest is kept for the next call).
        uint32_t TicksDue(double seconds, double tickSeconds, uint32_t maxTicks = 30);
        /// @brief Forgets ticks kept over a cap and not run yet (the prediction starts: its lead is
        /// set from the round trip, and ticks owed from before it would push it further ahead).
        void DropBacklog() { m_PendingJump = 0; }

        /**
         * @brief A snapshot's report: the input of tick `inputTick` arrived `margin` ticks before
         * the server needed it. `nextTick` is the tick this client predicts next and
         * `snapshotTick` the snapshot's (the hold timer counts server ticks).
         */
        ClockAction Report(int32_t margin, uint32_t inputTick, uint32_t nextTick, uint32_t snapshotTick);

        int32_t Target() const;
        double Rate() const { return m_Rate; }
        float Jitter() const { return m_Jitter; }
        bool HasReport() const { return m_HasPrevious; }
        /// Jumps made for late margins and for short ones (both in PredictionStats::clockJumps).
        uint32_t LateJumps() const { return m_LateJumps; }
        uint32_t ShortJumps() const { return m_ShortJumps; }

    private:
        ClockSettings m_Settings;
        double m_Accumulator = 0.0;
        double m_Rate = 1.0;
        uint32_t m_PendingJump = 0;
        uint32_t m_PendingHold = 0;
        uint32_t m_LateRun = 0;
        uint32_t m_ShortRun = 0;
        int32_t m_ShortHighest = 0;
        uint32_t m_LateJumps = 0, m_ShortJumps = 0;
        uint32_t m_IgnoreBefore = 0;
        bool m_HasPrevious = false;
        int32_t m_Previous = 0;
        uint32_t m_LastInputTick = 0;
        float m_Jitter = 0.0f;
        bool m_High = false;
        uint32_t m_HighSince = 0;
        std::deque<int32_t> m_Window;
    };

    /**
     * @brief The server's tick as this client estimates it, for drawing other players: it only
     * moves forward (review of stage 4: "newest snapshot + ticks since" jumped back on every late
     * snapshot).
     * @details `Advance` adds the time that passed (in ticks of real time, not the clock's
     * predicted ticks, which jump and hold); `Observe` takes a snapshot's tick when it arrives. The
     * estimate is the local time plus a smoothed offset (10 % of each new sample: about the mean
     * delay of the snapshots behind the server); `Now` runs towards it at 0.9-1.1 x real time and is
     * moved forward at once, never back, when it is more than 30 ticks behind (after a stall). It
     * stays at the first snapshot's tick while nothing advances it.
     */
    class ServerTime {
    public:
        static constexpr double kSmoothing = 0.1;
        static constexpr double kGain = 0.1;        ///< rate change per tick of error
        static constexpr double kMinRate = 0.9, kMaxRate = 1.1;
        static constexpr double kSnapTicks = 30.0;

        void Advance(double ticks);
        void Observe(uint32_t snapshotTick);
        double Now() const { return m_Now; }
        bool Started() const { return m_Started; }

    private:
        double m_Local = 0.0;
        double m_Offset = 0.0;
        double m_Now = 0.0;
        bool m_Started = false;
    };

    /// @brief One sample of another player (a snapshot's RemotePlayer at its tick).
    struct RemoteSample {
        uint32_t tick = 0;
        glm::vec3 position{0.0f};
        uint16_t yaw = 0;
        uint8_t flags = 0;
    };

    /// @brief Another player as this client draws it: interpolated in the past.
    struct RemotePlayerView {
        uint8_t slot = 0;
        glm::vec3 position{0.0f};
        uint16_t yaw = 0;
        uint8_t flags = 0;
    };

    /**
     * @brief The other players of the snapshots, drawn in the past (networking.md "Entity
     * replication": 8 samples; delay max(67 ms, 2 x the recent update interval), at most 200 ms;
     * extrapolation for up to one update interval, then hold). Players absent from a snapshot
     * have left. Linear between samples (players change direction at once).
     */
    class RemotePlayers {
    public:
        static constexpr size_t kSamples = 8;
        static constexpr double kMinDelayTicks = 4.0;    ///< 67 ms
        static constexpr double kMaxDelayTicks = 12.0;   ///< 200 ms

        /// @brief The others of a snapshot of `tick` (newer than every one before).
        void Add(uint32_t tick, const std::vector<RemotePlayer>& others);
        /// @brief Every player at server time `serverTick` (fractional ticks) minus its delay.
        std::vector<RemotePlayerView> Sample(double serverTick) const;
        size_t Count() const { return m_Tracks.size(); }

    private:
        struct Track {
            std::deque<RemoteSample> samples;
            double interval = 2.0;   ///< recent mean ticks between samples
        };
        std::map<uint8_t, Track> m_Tracks;
    };

    /// @brief A replicated body's pose at a server time (RemoteBodies::Sample).
    struct BodyPose {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 velocity{0.0f};
    };

    /**
     * @brief The awake replicated bodies of the snapshots, drawn in the past (networking.md "Entity
     * replication", stage 6): 8 `(tick, pose, velocity)` samples per body, Hermite between them
     * (position; rotation slerped); delay per body max(67 ms, 2 x its recent update interval), at most
     * 200 ms; extrapolation along the last velocity for up to one update interval, then hold.
     * @details `Rest(netId, tick)` (an EntityRest) drops the samples of `tick` or older and refuses
     * every such sample from then on; it reports whether the body rests (no newer sample is kept).
     * `MaxGap` is the largest gap in ticks between consecutive samples of one body while it moved,
     * and from its last sample to its rest (what the snapshot priority and the loss left; a rest in
     * between starts over). The rest state of a copy follows the events (EntityRest, EntityWake),
     * never the samples (net::Client takes a snapshot's samples only after its events).
     */
    class RemoteBodies {
    public:
        static constexpr size_t kSamples = 8;
        static constexpr double kMinDelayTicks = 4.0;    ///< 67 ms
        static constexpr double kMaxDelayTicks = 12.0;   ///< 200 ms

        /// @brief A snapshot's sample of `netId` at `tick`. False (ignored) when it is not newer than
        /// the newest sample or the body's rest.
        bool Add(uint32_t netId, uint32_t tick, const BodyPose& pose);
        /// @brief The body came to rest at `tick`: samples of `tick` or older dropped and refused from
        /// now on. True when the body rests (no newer sample is kept).
        bool Rest(uint32_t netId, uint32_t tick);
        /// @brief Forgets a body (despawned).
        void Remove(uint32_t netId);
        /// @brief Re-bases every sample of a body whose model origin moved by `localOffset` metres in
        /// its own frame (an ObjectModel).
        void Shift(uint32_t netId, const glm::vec3& localOffset);
        /// @brief The pose of `netId` at server time `serverTick` minus its delay; false without samples.
        bool Sample(uint32_t netId, double serverTick, double tickSeconds, BodyPose& out) const;
        bool HasSamples(uint32_t netId) const;
        uint32_t MaxGap() const { return m_MaxGap; }
        uint32_t MaxGap(uint32_t netId) const;
        /// @brief Samples of `netId` accepted since it was first seen (0 for an unknown body).
        uint32_t Received(uint32_t netId) const;
        size_t Count() const { return m_Tracks.size(); }

    private:
        struct Sample_ {
            uint32_t tick = 0;
            BodyPose pose;
        };
        struct Track {
            std::deque<Sample_> samples;
            double interval = 2.0;
            bool rested = false;          ///< an EntityRest came (restTick is valid)
            uint32_t restTick = 0;
            uint32_t maxGap = 0;
            uint32_t received = 0;
        };
        std::unordered_map<uint32_t, Track> m_Tracks;
        uint32_t m_MaxGap = 0;
    };
}
