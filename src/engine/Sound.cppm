module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

export module vortex.engine:sound;

import vortex.core;
import vortex.voxel;
import vortex.scene;
import vortex.sim;
import vortex.audio;

export namespace vortex {

    /// @brief What a world material sounds like (docs/design/audio.md "Events").
    enum class SoundMaterial : uint8_t { None, Grass, Dirt, Stone, Brick, Wood, Sand, Snow, Glass, Iron, Foliage, Water, Lava };
    inline constexpr uint32_t kSoundMaterialCount = 13;
    /// @brief By the material's name in the world palette ("stone", "planks", "granite" ...); None for an unknown one.
    SoundMaterial SoundMaterialOf(std::string_view materialName);

    /**
     * @brief The engine's sound presentation (docs/design/audio.md "Events"): blasts, breaks and collapses from a
     * presented blast; impacts of moving bodies; mechanisms, ropes and holds from the scene's own state. It reads
     * what the simulation already did and decides nothing in it, so a session client plays the same sounds from
     * the replicated events (its scene) as the offline game.
     */
    class SoundEffects {
    public:
        SoundEffects();
        /// @brief Resolves the sound IDs once (an ID missing from the manifest plays nothing).
        void Bind(audio::AudioSystem& audio, const voxel::MaterialLibrary& materials);
        bool Bound() const { return m_Audio != nullptr; }

        /// @brief The sounds of a presented blast: the core by radius, the break of its main material by the carved
        /// voxel count, patter, the crack of every larger island that broke off, and object damage.
        void Blast(const sim::BlastResult& blast, sim::Simulation& simulation);
        /// @brief Once per frame: delayed layers, body impacts (by material and speed), mechanism motion, rope cuts,
        /// pick-ups and throws.
        void Watch(sim::Simulation& simulation, float dt);
        /// @brief The footstep sound of a material (sound-list.md section 4), kNoSound when there is none.
        audio::SoundId Footstep(SoundMaterial material) const { return m_Foot[static_cast<size_t>(material)]; }
        /// @brief The material of a world palette entry, cached by id.
        SoundMaterial MaterialOf(uint8_t id) const { return m_ByMaterial[id]; }

        struct Stats {
            uint64_t blasts = 0, breaks = 0, islands = 0, impacts = 0, mechanisms = 0, ropes = 0, holds = 0;
        };
        const Stats& Counters() const { return m_Stats; }

    private:
        struct Body {
            glm::vec3 position{0.0f};
            glm::vec3 velocity{0.0f};
            uint32_t tick = 0;
            bool hasVelocity = false;
            bool seen = false;
        };
        struct Pending {
            double at = 0.0;
            audio::SoundId sound = audio::kNoSound;
            audio::PlayParams params;
        };
        struct Track {
            scene::MechanismSegment newest;
            bool landed = true;
            bool seen = false;
        };
        struct ModelInfo {
            std::weak_ptr<const voxel::VoxelModel> model;   // the cache is by address: a freed model's may be reused
            SoundMaterial material = SoundMaterial::None;
            uint32_t voxels = 0;
            bool crate = false;
        };

        void Play(audio::SoundId sound, const glm::dvec3& at, float volumeDb = 0.0f, float pitch = 0.0f);
        void Later(double seconds, audio::SoundId sound, const glm::dvec3& at, float volumeDb = 0.0f);
        ModelInfo InfoOf(const std::shared_ptr<voxel::VoxelModel>& model, const voxel::MaterialLibrary& materials);
        void WatchBodies(sim::Simulation& simulation);
        void WatchMechanisms(sim::Simulation& simulation);
        void WatchRopes(sim::Simulation& simulation);
        void WatchHolds(sim::Simulation& simulation);

        audio::AudioSystem* m_Audio = nullptr;
        std::array<SoundMaterial, 256> m_ByMaterial{};
        std::array<audio::SoundId, kSoundMaterialCount> m_Foot{}, m_BreakSmall{}, m_BreakLarge{}, m_ImpactLight{}, m_ImpactHeavy{};
        audio::SoundId m_CoreSmall = audio::kNoSound, m_CoreMedium = audio::kNoSound, m_CoreLarge = audio::kNoSound;
        audio::SoundId m_Patter = audio::kNoSound, m_CrackStone = audio::kNoSound, m_CrackWood = audio::kNoSound;
        audio::SoundId m_Groan = audio::kNoSound, m_Rumble = audio::kNoSound, m_Land = audio::kNoSound, m_Slump = audio::kNoSound;
        audio::SoundId m_Settle = audio::kNoSound, m_CrateImpact = audio::kNoSound, m_CrateBreak = audio::kNoSound;
        audio::SoundId m_DoorOpen = audio::kNoSound, m_DoorClose = audio::kNoSound, m_DoorUnlatch = audio::kNoSound;
        audio::SoundId m_DoorFreed = audio::kNoSound, m_DoorJammed = audio::kNoSound;
        audio::SoundId m_PortRaise = audio::kNoSound, m_PortDrop = audio::kNoSound, m_PortLand = audio::kNoSound;
        audio::SoundId m_LeverThrow = audio::kNoSound, m_LeverReturn = audio::kNoSound, m_LeverBreak = audio::kNoSound;
        audio::SoundId m_RopeSnap = audio::kNoSound, m_ChainSnap = audio::kNoSound, m_Pickup = audio::kNoSound;

        std::unordered_map<uint32_t, Body> m_Bodies;           // by entity
        std::unordered_map<uint32_t, Track> m_Mechanisms;      // by entity
        std::unordered_map<uint32_t, bool> m_RopeCut;          // by entity
        std::unordered_map<uint32_t, bool> m_Held;             // by entity: still held this frame
        std::unordered_map<const voxel::VoxelModel*, ModelInfo> m_Models;
        std::vector<Pending> m_Pending;
        double m_Now = 0.0;
        Stats m_Stats;
    };
}
