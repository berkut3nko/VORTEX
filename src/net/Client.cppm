module;

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

export module vortex.net:client;

import vortex.voxel;
import vortex.sim;
import :transport;
import :messages;
import :session;

export namespace vortex::net {

    struct ClientSettings {
        Token token{};
        /// Hello::buildHash to send; 0 = BuildHash of the default world materials (what
        /// sim::Simulation::Initialize registers). Attach checks the simulation against it.
        uint64_t buildHash = 0;
        /// Keep the applied operations for the owner to show (TakePresentations -> sim::PresentBlast).
        bool presentation = false;
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
        uint32_t statusReceived = 0;
        /// Chunks an operation could not change on this client (not loaded / no store slot): until
        /// networking stage 7 a divergence, so the first one fails the client (the window is loaded
        /// before anything is applied and the store sized for it, so it does not happen).
        uint32_t skippedChunks = 0;
        uint32_t unsyncedChunks = 0;
        uint32_t probeMismatches = 0;     ///< Probe chunks this client generates differently (stage 7: server terrain mode)
    };

    /**
     * @brief A session client (docs/design/networking.md stage 3): handshake, then the region from
     * `Welcome`, then the opSeq stream applied in order with the same sim::ApplyOp the server ran.
     * Terrain is never predicted and no operation is ever sent.
     * @details The owner creates the client's Simulation once the state is Welcomed, from
     * MakeSimulationConfig and MakeGenerator (never from its own settings: the voxel size, window
     * and height range are the session's), and gives it to Attach. Events that arrive before are
     * kept. Call Poll once per tick after the streaming phase; with `presentation` the applied
     * operations are grouped per cast for sim::PresentBlast.
     */
    class Client {
    public:
        Client(ClientSettings settings, ITransport& transport);
        ~Client();
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        /// @brief Receives and applies whatever arrived.
        void Poll();

        ClientState State() const;
        RefuseReason RefusalReason() const;
        const sim::RegionDesc& Region() const;            ///< From Welcomed on.
        glm::dvec3 Spawn() const;
        uint8_t Slot() const;
        /// @brief `base` with the session's world: memory only, the region's voxel size, window and
        /// height range (sim::RegionWorldConfig), and a store of at least sim::RegionChunkCapacity
        /// slots (the client holds the whole window until stage 7). Valid from Welcomed on.
        sim::SimulationConfig MakeSimulationConfig(sim::SimulationConfig base = {}) const;
        /// @brief The region's generator with the default world material ids.
        std::shared_ptr<voxel::IWorldGenerator> MakeGenerator() const;
        /**
         * @brief Takes the simulation the events are applied to: refused (false, logged; the state
         * stays Welcomed) when it is not initialised, has a save directory (a session world is
         * memory only), or its voxel size, window, height range or material table differ from the
         * session's. Loads the whole window (World::LoadBlocking) and refuses it when the window did
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
