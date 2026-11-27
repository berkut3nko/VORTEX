module;

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module vortex.net:session;

import vortex.voxel;
import vortex.sim;
import :messages;

export namespace vortex::net {

    /**
     * @brief A session as the server is started with it (docs/design/networking.md stage 3; M7
     * replaces it with matchmaking). Read from a text file of `key = value` lines (LoadSessionFile).
     */
    struct SessionConfig {
        Token token{};                    ///< `token`: 32 hex digits, compared in plain text.
        sim::RegionDesc region;           ///< `template`, `seed`, `play_min`, `play_max`, `height`, `voxel_size`, `generator`, `flat_ground`, `terrain_seed`
        /// `land_map`: with `generator = land`, the folder of the drawn world map (the game's assets/world: height.png,
        /// biome_ids.png, region_ids.png, world.json, points.json), relative to the working directory. LoadSessionFile
        /// bakes it and attaches the map to `region` (sim::SetRegionLand).
        std::string landMap;
        uint32_t maxClients = 16;         ///< `max_clients`
        /// `chunk_capacity`: store slots of the server's world. 0 = sim::RegionChunkCapacity: every
        /// cell of the window (the store allocates a chunk only when a slot is used), capped at 2 GiB
        /// of chunks (sim::kMaxRegionStoreSlots) so that a full store, the designed failure, comes
        /// before the process runs out of memory.
        uint32_t chunkCapacity = 0;
        bool packHidden = true;           ///< `pack_hidden`: buried chunks keep only their RLE bytes (memory only).
        /// `mechanisms`: with the playground template, the mechanism yard (a door, a gate and its lever) is built at the
        /// start and replicated ([mech-1], docs/design/mechanisms.md "Replication"). The hollowkeep template's portcullis
        /// and lever are built whatever this says.
        bool mechanisms = false;
        uint16_t port = 0;                ///< `port`: UDP port (stage 5; unused on a loopback).
        /// [wp-7] The fight (docs/design/fight.md "Rules"): `mode`, `team_spawn_1`, `team_spawn_2`, `score_limit`,
        /// `round_seconds`, `respawn_ticks`, `charge_ticks`, `max_charges`, `min_players`, `countdown_ticks`,
        /// `end_ticks`, `tower_1`, `tower_2`, `end_session`, `hold_seconds` (siege: needs `template = hollowkeep`). A
        /// sandbox session (the default) is sim::SandboxRules() whatever the other keys say.
        sim::FightRules fight = sim::SandboxRules();
    };

    /// @brief Parses a session file's text. Unknown keys and malformed values are errors (`error`
    /// names the line); the region is validated with sim::IsValid.
    bool ParseSession(std::string_view text, SessionConfig& out, std::string& error);
    bool LoadSessionFile(const std::filesystem::path& path, SessionConfig& out, std::string& error);

    /// @brief With `generator = land`: bakes the map in `config.landMap` and attaches it (sim::SetRegionLand);
    /// LoadSessionFile calls it. True with nothing to do for other generators.
    bool BakeSessionLand(SessionConfig& config, std::string& error);
    /// @brief A session file that ParseSession reads back to `config` (floats written exactly).
    std::string FormatSession(const SessionConfig& config);
    /// @brief 32 hex digits -> 16 bytes.
    bool ParseToken(std::string_view hex, Token& out);
    std::string FormatToken(const Token& token);

    /**
     * @brief Hello::buildHash of this process: kProtocolVersion, voxel::kGeneratorGoldenHash (the
     * generator the build was made with), voxel::MaterialTableHash(materials) (what carves,
     * island searches and the load check read; shred.md R6) and sim::kPlayerRuleRevision (the
     * movement rule: a client and a server that would move a player differently refuse each
     * other instead of correcting each other) and voxel::kLandGoldenHash (the world map's bake and
     * sampling: land.md "Decision" 3) and the magic tables (sim::SpellTableHash of the table at
     * voxel::kDefaultVoxelSize, voxel::TurningTableHash of the world palette's table; magic.md
     * "Wire values"). Two builds that may share a session have equal values.
     * = BuildHashOf(kProtocolVersion, kGeneratorGoldenHash, MaterialTableHash(materials),
     * kPlayerRuleRevision, kLandGoldenHash, SpellTableHash, TurningTableHash).
     */
    uint64_t BuildHash(const voxel::MaterialLibrary& materials);
    /// @brief The hash BuildHash mixes; a player rule revision of 0 (builds before it existed), a
    /// land golden hash of 0 (builds before the world map) and table hashes of 0 (builds before
    /// [mag-2]) mix nothing.
    uint64_t BuildHashOf(uint32_t protocol, uint64_t generatorGolden, uint64_t materialTable, uint32_t playerRule,
                         uint64_t landGolden = 0, uint64_t spellTable = 0, uint64_t turningTable = 0);

    /// Probe chunks per session (networking.md "Region start-up": 64).
    inline constexpr uint32_t kProbeChunks = 64;
    /**
     * @brief `count` chunks of the region's window chosen from the region's seed and their
     * pristine hashes (generator output, never the edited world). Columns are uniform over the
     * window; the row is the column's lowest surface row or the one above when the generator can
     * classify columns (so most probes hold terrain), else uniform over the height range.
     */
    std::vector<ProbeHash> ProbeChunks(const sim::RegionDesc& region, const voxel::IWorldGenerator& generator,
                                       uint32_t count = kProbeChunks);
    /// @brief Probes whose hash this generator does not reproduce.
    uint32_t CheckProbes(const std::vector<ProbeHash>& probes, const voxel::IWorldGenerator& generator);
    /// @brief The same chunks with the hashes this generator gives them.
    std::vector<ProbeHash> RehashProbes(const std::vector<ProbeHash>& probes, const voxel::IWorldGenerator& generator);
    /// @brief Probes whose chunk in `world` (loaded, Packed ones read through) does not have the
    /// probe's hash; a chunk that is not loaded counts as differing.
    uint32_t CheckProbes(const std::vector<ProbeHash>& probes, const voxel::World& world);

    /**
     * @brief A session client's generator (networking stage 7b, server terrain mode): the region's
     * generator (as the client's build makes it) until terrain mode starts; from then on every chunk
     * inside the server's window is Empty at once - no dense generation, the client's terrain comes
     * from the server (TerrainAck and ChunkData). Column classes and vertical bounds are still the inner
     * generator's (cheap; the server's ack overrides whatever they place). Chunks outside the server's
     * window are generated as before (never edited, never sent). Thread safe: the switch is an atomic
     * read by the World's jobs; a job already running finishes with the old answer, which the
     * client's reset and the installs' new cell generation make harmless.
     */
    class SessionGenerator final : public voxel::IWorldGenerator {
    public:
        SessionGenerator(std::shared_ptr<voxel::IWorldGenerator> inner, const glm::ivec3& serverMin, const glm::ivec3& serverMax)
            : m_Inner(std::move(inner)), m_Min(serverMin), m_Max(serverMax) {}
        voxel::ChunkContent Generate(const glm::ivec3& coord, voxel::Chunk& chunk) const override;
        voxel::HeightBounds VerticalBounds() const override { return m_Inner->VerticalBounds(); }
        voxel::ColumnClass ClassifyColumn(int cx, int cz) const override { return m_Inner->ClassifyColumn(cx, cz); }
        void SetTerrainMode(bool on) { m_TerrainMode.store(on); }
        bool TerrainMode() const { return m_TerrainMode.load(); }
        /// @brief Chunks inside the server's window answered Empty in terrain mode, and chunks there
        /// the inner generator made a Dense chunk for while terrain mode was on (0: a job that ran
        /// across the switch is the only way; tests assert 0 for a client in terrain mode from the
        /// Welcome).
        uint64_t Skipped() const { return m_Skipped.load(); }
        uint64_t DenseInsideInTerrainMode() const { return m_DenseInside.load(); }
        const voxel::IWorldGenerator& Inner() const { return *m_Inner; }

    private:
        std::shared_ptr<voxel::IWorldGenerator> m_Inner;
        glm::ivec3 m_Min{0}, m_Max{0};    ///< the server's window, [min, max)
        std::atomic<bool> m_TerrainMode{false};
        mutable std::atomic<uint64_t> m_Skipped{0}, m_DenseInside{0};
    };

    /**
     * @brief Tests and vortex_server's broken bots (stage 7b, `--bot-broken-chunk`): a generator that
     * makes `inner`'s terrain except in `chunks`, where 64 voxels spread over the chunk get another id
     * (air becomes stone, solid becomes air; a uniform chunk becomes a dense one). A chunk the world
     * places from its column classes without generating it (sky and rock rows) is not broken: break
     * the generated surface band. Column classes and vertical bounds are `inner`'s.
     */
    std::shared_ptr<voxel::IWorldGenerator> BreakChunks(std::shared_ptr<voxel::IWorldGenerator> inner, std::vector<glm::ivec3> chunks,
                                                        uint8_t stoneId);

    /// @brief WorldHash over the region's window (RegionWindowMinChunk / RegionWindowSize, the
    /// full-height rows): what server and clients compare.
    voxel::WorldHashResult RegionHash(const voxel::World& world, const sim::RegionDesc& region);
}
