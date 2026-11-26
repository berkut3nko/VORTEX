module;

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
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

    /**
     * @brief One bit per voxel of a box: which voxels an operation touches (World::ClearMask, a
     * terrain island that broke off).
     * @details Bits follow VoxelVolume::Index order (x fastest, then y, then z) and are packed eight
     * to a byte, lowest bit first, so the bytes are the same on every machine and can be sent as
     * they are (or run-length encoded). Bits past the last voxel are always zero.
     */
    class VoxelMask {
    public:
        VoxelMask() = default;
        /// @brief All bits clear. Negative components count as 0.
        explicit VoxelMask(const glm::ivec3& size);
        /// @brief A bit for every solid voxel of `volume`.
        static VoxelMask FromVolume(const VoxelVolume& volume);

        const glm::ivec3& Size() const { return m_Size; }
        size_t VoxelCount() const {
            return static_cast<size_t>(m_Size.x) * static_cast<size_t>(m_Size.y) * static_cast<size_t>(m_Size.z);
        }
        bool InBounds(int x, int y, int z) const {
            return x >= 0 && y >= 0 && z >= 0 && x < m_Size.x && y < m_Size.y && z < m_Size.z;
        }
        size_t Index(int x, int y, int z) const {
            return static_cast<size_t>(x) + static_cast<size_t>(m_Size.x) * (static_cast<size_t>(y) + static_cast<size_t>(m_Size.y) * static_cast<size_t>(z));
        }
        /// @brief false outside the box.
        bool Get(int x, int y, int z) const {
            if (!InBounds(x, y, z)) return false;
            const size_t i = Index(x, y, z);
            return ((m_Bits[i >> 3] >> (i & 7u)) & 1u) != 0;
        }
        bool Get(const glm::ivec3& p) const { return Get(p.x, p.y, p.z); }
        /// @brief Writes a bit; writes outside the box are ignored.
        void Set(int x, int y, int z, bool on);
        void Set(const glm::ivec3& p, bool on) { Set(p.x, p.y, p.z, on); }
        /// @brief Number of set bits.
        uint32_t Count() const;
        /// @brief True if any bit with an Index in [first, last) is set (clamped to the mask).
        bool AnyInRange(size_t first, size_t last) const;
        /// @brief Sets every bit with an Index in [first, last) (clamped to the mask): a decoded run.
        void SetRange(size_t first, size_t last);
        /// @brief The packed bits: (VoxelCount() + 7) / 8 bytes.
        const std::vector<uint8_t>& Bytes() const { return m_Bits; }

        friend bool operator==(const VoxelMask&, const VoxelMask&) = default;

    private:
        glm::ivec3 m_Size{0};
        std::vector<uint8_t> m_Bits;
    };

    // --- mask run encoding ----------------------------------------------------------------------
    //  A mask as alternating clear / set run lengths, the first run clear (it may be 0). The run
    //  axis is the fastest; the other two follow in x, y, z order: axis 0 visits x, y, z; axis 1
    //  y, x, z; axis 2 z, x, y (fastest first). The axis changes only the order, never the voxels.

    /// @brief The runs of `mask` in the order of `axis` (0 x, 1 y, 2 z). Empty when axis > 2.
    std::vector<uint32_t> MaskRuns(const VoxelMask& mask, uint8_t axis);
    /// @brief The inverse of MaskRuns. False (and `out` an empty mask of `size`) when axis > 2, a
    /// size is negative, or the runs do not add up to the voxel count exactly.
    bool MaskFromRuns(const glm::ivec3& size, uint8_t axis, std::span<const uint32_t> runs, VoxelMask& out);
    /// @brief Bytes of MaskRuns(mask, axis) as LEB128 numbers (0 when axis > 2).
    size_t MaskRunBytes(const VoxelMask& mask, uint8_t axis);
    /// @brief The axis whose runs take the fewest bytes (a tie goes to the lower axis).
    uint8_t ShortestRunAxis(const VoxelMask& mask);
}
