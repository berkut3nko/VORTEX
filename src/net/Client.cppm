module;

#include <glm/glm.hpp>

#include <cstdint>
#include <array>
#include <memory>
#include <vector>

export module vortex.net:client;

import vortex.voxel;
import vortex.sim;
import :transport;
import :messages;
import :prediction;
import :session;

export namespace vortex::net {

    struct ClientSettings {
        Token token{};
        /// Hello::buildHash to send; 0 = BuildHash of the default world materials (what
        /// sim::Simulation::Initialize registers). Attach checks the simulation against it.
        uint64_t buildHash = 0;
        /// Keep the applied operations for the owner to show (TakePresentations -> sim::PresentBlast).
        bool presentation = false;
        /// Inputs per packet at most (the newest unacknowledged ones) and one packet every this many
        /// ticks (networking.md: 30 Hz packets, at most 8 inputs).
        uint32_t maxInputsPerPacket = 8;
        uint32_t inputSendInterval = 2;
        ClockSettings clock;
        /// Reconciliation (networking.md "Player movement"): an error below `ignoreError` metres is
        /// left alone; above it the client restores the server's state and replays its inputs, and
        /// blends the visual jump out over `blendTicks`; above `snapError` it snaps.
        float ignoreError = 0.02f;
        float snapError = 1.5f;
        uint32_t blendTicks = 6;
        /// A snapshot whose events have not arrived this many ticks (250 ms) after it did is given
        /// up: counted, no replay (the newest snapshot whose events are applied is compared meanwhile).
        uint32_t eventWaitTicks = 15;
    };

    enum class ClientState : uint8_t {
        Connecting,   ///< waiting for the transport's Connected
        Joining,      ///< Hello sent
        Welcomed,     ///< the region is known: build a Simulation from it and Attach
        Playing,      ///< attached: events are applied as they arrive
        Refused,      ///< the server said no (RefusalReason)
        Closed,       ///< disconnected
        Failed,       ///< a malformed message, a gap in the stream, an invalid region, an operation it could not apply in full (logged; it disconnects)
    };

    struct ClientStats {
        uint64_t eventsApplied = 0;
        uint64_t opsApplied = 0;
        uint64_t lightsSpawned = 0;
        uint64_t duplicateEvents = 0;     ///< opSeq already applied: ignored
        uint64_t eventBytes = 0;          ///< Event batch bytes received
        uint32_t snapshotsReceived = 0;
        uint32_t snapshotsStale = 0;      ///< older than one already received (reordered or duplicated): only their margin is ignored too
        uint32_t malformedSnapshots = 0;  ///< state-channel messages that are not a valid snapshot (ignored)
        /// Chunks an operation could not change on this client (not loaded / no store slot): until
        /// networking stage 7 a divergence, so the first one fails the client (the window is loaded
        /// before anything is applied and the store sized for it, so it does not happen).
        uint32_t skippedChunks = 0;
        uint32_t unsyncedChunks = 0;
        uint32_t probeMismatches = 0;     ///< Probe chunks this client generates differently (stage 7: server terrain mode)
    };

    /// @brief How the own player's prediction went (networking.md stage 4: "correction rate logged").
    struct PredictionStats {
        uint64_t ticks = 0;               ///< Ticks predicted (StepPlayer calls that moved the player or held it).
        uint64_t heldTicks = 0;           ///< ... held by the gate (terrain without a collider)
        uint64_t inputPackets = 0, inputsSent = 0;
        uint64_t compared = 0;            ///< Snapshots compared with the prediction.
        uint64_t within = 0;              ///< ... whose error was under ignoreError: left alone
        uint64_t corrections = 0;         ///< ... restored and replayed (error at or above ignoreError)
        uint64_t snaps = 0;               ///< ... of them above snapError: no blend
        /// Errors by size: < 1 mm, < 2 cm, < 5 cm, < 20 cm, < 1.5 m, more.
        std::array<uint64_t, 6> errorBuckets{};
        float maxError = 0.0f, lastError = 0.0f;
        uint64_t replayTicks = 0;         ///< StepPlayer calls of replays
        uint32_t maxReplayTicks = 0;
        double replaySeconds = 0.0, maxReplaySeconds = 0.0;   ///< wall clock (logged only)
        uint64_t waitedForEvents = 0;     ///< Snapshots compared only after their events arrived
        uint64_t eventTimeouts = 0;       ///< Snapshots given up eventWaitTicks after they arrived (not compared)
        uint64_t tooOld = 0;              ///< Snapshots older than the kept history (not compared)
        uint64_t behind = 0;              ///< Snapshots at or past the tick being predicted: the state was taken as it is
        uint32_t clockJumps = 0, clockJumpTicks = 0;
        uint32_t hardResyncs = 0, resyncHoldTicks = 0;
        uint64_t bleedTicks = 0;          ///< Ticks run while the clock bled a surplus
        uint64_t catchUpTicks = 0;        ///< Ticks run while the clock made up a deficit
        int32_t lastMargin = 0, minMargin = 0, maxMargin = 0;
        bool anyMargin = false;
        /// Corrections per minute of prediction.
        double CorrectionsPerMinute(double tickSeconds) const {
            return ticks == 0 ? 0.0 : static_cast<double>(corrections) * 60.0 / (static_cast<double>(ticks) * tickSeconds);
        }
    };

    /**
     * @brief A session client (docs/design/networking.md stages 3-4): handshake, then the region
     * from `Welcome`, then the opSeq stream applied in order with the same sim::ApplyOp the server
     * ran, and the own player predicted with sim::StepPlayer and reconciled with the server's
     * snapshots. Terrain is never predicted and no operation is ever sent.
     * @details The owner creates the client's Simulation once the state is Welcomed, from
     * MakeSimulationConfig and MakeGenerator (never from its own settings: the voxel size, window
     * and height range are the session's), and gives it to Attach. Events that arrive before are
     * kept. With `presentation` the applied operations are grouped per cast for sim::PresentBlast.
     *
     * Driving it, per frame of the owner: `n = TicksDue(seconds)` (the clock, kept ahead of the
     * server), then n ticks of: the simulation's streaming phase, `Poll` (events, snapshots,
     * reconciliation), `StepPlayer(controls)` (the predicted tick; it sends the inputs), the
     * simulation's physics and lifetime phases. `RunTick` is that tick on the attached simulation.
     */
    class Client {
    public:
        Client(ClientSettings settings, ITransport& transport);
        ~Client();
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        /// @brief Receives and applies whatever arrived; compares the newest snapshot whose events
        /// are applied with the prediction and corrects it.
        void Poll();

        /// @brief The clock: ticks to run now for `seconds` of the owner's time (rate 1 before the
        /// prediction starts; then kept ahead of the server: jumps, a 2 % bleed, holds).
        uint32_t TicksDue(double seconds);
        /**
         * @brief One predicted tick of the own player, between the simulation's streaming and
         * physics phases: `controls` (its `tick` is ignored) become the input of the next tick,
         * sim::StepPlayer moves the player, the input and the state are kept for replays, and every
         * inputSendInterval ticks the unacknowledged inputs are sent. Before the first snapshot
         * (the server's state of the player) nothing happens.
         */
        sim::PlayerStepResult StepPlayer(const sim::PlayerInput& controls);
        /// @brief One whole tick on the attached simulation: streaming (colliders around the own
        /// player), Poll, StepPlayer, physics, lifetimes. Before Attach: Poll only.
        sim::PlayerStepResult RunTick(const sim::PlayerInput& controls);

        bool Predicting() const;
        /// @brief The predicted state of the own player (after the last StepPlayer).
        const sim::PlayerState& Player() const;
        /// @brief Where to draw the own player: the prediction plus the correction being blended out.
        glm::vec3 PlayerViewPosition() const;
        /// @brief The server tick the next StepPlayer predicts.
        uint32_t PredictTick() const;
        /// @brief The other players at the server time this client draws them (in the past):
        /// net::ServerTime, advanced by TicksDue, so it only moves forward.
        std::vector<RemotePlayerView> RemotePlayers() const;
        /// @brief The session's player tuning (from Welcome).
        const sim::PlayerTuning& Tuning() const;
        const PredictionStats& Prediction() const;
        const TickClock& Clock() const;

        ClientState State() const;
        RefuseReason RefusalReason() const;
        const sim::RegionDesc& Region() const;            ///< From Welcomed on.
        glm::dvec3 Spawn() const;
        uint8_t Slot() const;
        /// @brief `base` with the session's world: memory only, the region's voxel size, window and
        /// height range (sim::RegionWorldConfig), a store of at least sim::RegionChunkCapacity slots
        /// (the client holds the whole window until stage 7), the server's tick length and gravity
        /// (Welcome), and the session's character mask (sim::kSessionCharacterLayers: terrain only
        /// until stage 6, as on the server). Valid from Welcomed on.
        sim::SimulationConfig MakeSimulationConfig(sim::SimulationConfig base = {}) const;
        /// @brief The region's generator with the default world material ids.
        std::shared_ptr<voxel::IWorldGenerator> MakeGenerator() const;
        /**
         * @brief Takes the simulation the events are applied to: refused (false, logged; the state
         * stays Welcomed) when it is not initialised, has a save directory (a session world is
         * memory only), or its voxel size, window, height range, material table, tick length,
         * gravity or character mask differ from the session's. Loads the whole window (World::LoadBlocking) and refuses it when the window did
         * not land on the region's, the store filled, or a probe chunk is not what this client's
         * generator makes (another generator, or edited before Attach). Then applies the events
         * received so far.
         */
        bool Attach(sim::Simulation& simulation);

        uint32_t AppliedOpSeq() const;    ///< The last event applied.
        uint32_t ServerOpSeq() const;     ///< The last event the server said it made (status or batch).
        uint32_t LogEnd() const;          ///< opSeq the join log ends at (Welcome).
        const ClientStats& Stats() const;
        /// @brief The applied live operations (after LogEnd) since the last call, one BlastResult per
        /// cast (a carve and the Detach / ClearMask after it; `deferred` when there is no carve).
        /// Empty without `presentation`; the join log is never presented.
        std::vector<sim::BlastResult> TakePresentations();
        voxel::WorldHashResult Hash() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
