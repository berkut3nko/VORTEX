module;

#include <cstdint>
#include <filesystem>
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
        uint32_t maxClients = 16;         ///< `max_clients`
        /// `chunk_capacity`: store slots of the server's world. 0 = sim::RegionChunkCapacity: every
        /// cell of the window (the store allocates a chunk only when a slot is used), capped at 2 GiB
        /// of chunks (sim::kMaxRegionStoreSlots) so that a full store, the designed failure, comes
        /// before the process runs out of memory.
        uint32_t chunkCapacity = 0;
        bool packHidden = true;           ///< `pack_hidden`: buried chunks keep only their RLE bytes (memory only).
        uint16_t port = 0;                ///< `port`: UDP port (stage 5; unused on a loopback).
    };

    /// @brief Parses a session file's text. Unknown keys and malformed values are errors (`error`
    /// names the line); the region is validated with sim::IsValid.
    bool ParseSession(std::string_view text, SessionConfig& out, std::string& error);
    bool LoadSessionFile(const std::filesystem::path& path, SessionConfig& out, std::string& error);
    /// @brief A session file that ParseSession reads back to `config` (floats written exactly).
    std::string FormatSession(const SessionConfig& config);
    /// @brief 32 hex digits -> 16 bytes.
    bool ParseToken(std::string_view hex, Token& out);
    std::string FormatToken(const Token& token);

    /**
     * @brief Hello::buildHash of this process: kProtocolVersion, voxel::kGeneratorGoldenHash (the
     * generator the build was made with) and voxel::MaterialTableHash(materials) (what carves,
     * island searches and the load check read; shred.md R6). Two builds that may share a
     * session have equal values.
     */
    uint64_t BuildHash(const voxel::MaterialLibrary& materials);

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

    /// @brief WorldHash over the region's window (RegionWindowMinChunk / RegionWindowSize, the
    /// full-height rows): what server and clients compare.
    voxel::WorldHashResult RegionHash(const voxel::World& world, const sim::RegionDesc& region);
}
