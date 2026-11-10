module;

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

export module vortex.voxel:volume;

export namespace vortex::voxel {

    /**
     * @brief Dense voxel grid of arbitrary size: the authoring / gameplay representation of a
     * voxel object (prop, character part, debris, building ...).
     * @details Algorithms that reason about whole objects (connectivity, colliders, mass, import)
     * work on a VoxelVolume because plain x/y/z addressing is the simplest thing to get right.
     * For rendering and ray queries a volume is "baked" into 32^3 chunks (see VoxelModel).
     */
    class VoxelVolume {
    public:
        VoxelVolume() = default;
        explicit VoxelVolume(const glm::ivec3& size);

        const glm::ivec3& Size() const { return m_Size; }
        bool InBounds(int x, int y, int z) const {
            return x >= 0 && y >= 0 && z >= 0 && x < m_Size.x && y < m_Size.y && z < m_Size.z;
        }
        size_t Index(int x, int y, int z) const {
            return static_cast<size_t>(x) + static_cast<size_t>(m_Size.x) * (static_cast<size_t>(y) + static_cast<size_t>(m_Size.y) * static_cast<size_t>(z));
        }

        /// @brief Material id, 0 outside the volume.
        uint8_t Get(int x, int y, int z) const { return InBounds(x, y, z) ? m_Data[Index(x, y, z)] : uint8_t{0}; }
        uint8_t Get(const glm::ivec3& p) const { return Get(p.x, p.y, p.z); }
        /// @brief Writes a voxel; writes outside the volume are ignored.
        void Set(int x, int y, int z, uint8_t id) {
            if (InBounds(x, y, z)) m_Data[Index(x, y, z)] = id;
        }
        void Set(const glm::ivec3& p, uint8_t id) { Set(p.x, p.y, p.z, id); }

        std::vector<uint8_t>& Data() { return m_Data; }
        const std::vector<uint8_t>& Data() const { return m_Data; }

        uint32_t CountSolid() const;
        bool IsEmpty() const { return CountSolid() == 0; }

        /// @brief Tight bounds of the solid voxels: [min, max). Returns false if the volume is empty.
        bool SolidBounds(glm::ivec3& outMin, glm::ivec3& outMax) const;
        /// @brief Copy of the region [min, max).
        VoxelVolume Cropped(const glm::ivec3& min, const glm::ivec3& max) const;

        /// @brief Fills the box [min, max) (clipped to the volume).
        void FillBox(const glm::ivec3& min, const glm::ivec3& max, uint8_t id);
        /// @brief Fills voxels whose centre lies within `radius` of `center` (voxel units).
        void FillSphere(const glm::vec3& center, float radius, uint8_t id);

    private:
        glm::ivec3 m_Size{0};
        std::vector<uint8_t> m_Data;
    };
}
