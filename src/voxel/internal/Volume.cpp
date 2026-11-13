module;

#include <glm/glm.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

module vortex.voxel;

namespace vortex::voxel {

    VoxelVolume::VoxelVolume(const glm::ivec3& size) : m_Size(glm::max(size, glm::ivec3(0))) {
        m_Data.assign(static_cast<size_t>(m_Size.x) * static_cast<size_t>(m_Size.y) * static_cast<size_t>(m_Size.z), 0);
    }

    uint32_t VoxelVolume::CountSolid() const {
        uint32_t n = 0;
        for (uint8_t v : m_Data) n += v != 0;
        return n;
    }

    bool VoxelVolume::SolidBounds(glm::ivec3& outMin, glm::ivec3& outMax) const {
        glm::ivec3 mn(m_Size), mx(-1);
        size_t i = 0;
        for (int z = 0; z < m_Size.z; ++z)
            for (int y = 0; y < m_Size.y; ++y)
                for (int x = 0; x < m_Size.x; ++x, ++i) {
                    if (m_Data[i] == 0) continue;
                    mn = glm::min(mn, glm::ivec3(x, y, z));
                    mx = glm::max(mx, glm::ivec3(x, y, z));
                }
        if (mx.x < 0) return false;
        outMin = mn;
        outMax = mx + glm::ivec3(1);
        return true;
    }

    VoxelVolume VoxelVolume::Cropped(const glm::ivec3& min, const glm::ivec3& max) const {
        const glm::ivec3 mn = glm::clamp(min, glm::ivec3(0), m_Size);
        const glm::ivec3 mx = glm::clamp(max, mn, m_Size);
        VoxelVolume out(mx - mn);
        for (int z = mn.z; z < mx.z; ++z)
            for (int y = mn.y; y < mx.y; ++y)
                for (int x = mn.x; x < mx.x; ++x) out.m_Data[out.Index(x - mn.x, y - mn.y, z - mn.z)] = m_Data[Index(x, y, z)];
        return out;
    }

    void VoxelVolume::FillBox(const glm::ivec3& min, const glm::ivec3& max, uint8_t id) {
        const glm::ivec3 mn = glm::clamp(min, glm::ivec3(0), m_Size);
        const glm::ivec3 mx = glm::clamp(max, glm::ivec3(0), m_Size);
        for (int z = mn.z; z < mx.z; ++z)
            for (int y = mn.y; y < mx.y; ++y)
                for (int x = mn.x; x < mx.x; ++x) m_Data[Index(x, y, z)] = id;
    }

    void VoxelVolume::FillSphere(const glm::vec3& center, float radius, uint8_t id) {
        const glm::ivec3 mn = glm::clamp(glm::ivec3(glm::floor(center - glm::vec3(radius))), glm::ivec3(0), m_Size);
        const glm::ivec3 mx = glm::clamp(glm::ivec3(glm::ceil(center + glm::vec3(radius))) + glm::ivec3(1), glm::ivec3(0), m_Size);
        const float r2 = radius * radius;
        for (int z = mn.z; z < mx.z; ++z)
            for (int y = mn.y; y < mx.y; ++y)
                for (int x = mn.x; x < mx.x; ++x) {
                    const glm::vec3 d = glm::vec3(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f) - center;
                    if (glm::dot(d, d) <= r2) m_Data[Index(x, y, z)] = id;
                }
    }

    // --- VoxelMask -----------------------------------------------------------------------------

    VoxelMask::VoxelMask(const glm::ivec3& size) : m_Size(glm::max(size, glm::ivec3(0))) {
        m_Bits.assign((VoxelCount() + 7) / 8, 0);
    }

    VoxelMask VoxelMask::FromVolume(const VoxelVolume& volume) {
        VoxelMask mask(volume.Size());
        const std::vector<uint8_t>& data = volume.Data();
        for (size_t i = 0; i < data.size(); ++i) {
            if (data[i] != 0) mask.m_Bits[i >> 3] = static_cast<uint8_t>(mask.m_Bits[i >> 3] | (1u << (i & 7u)));
        }
        return mask;
    }

    void VoxelMask::Set(int x, int y, int z, bool on) {
        if (!InBounds(x, y, z)) return;
        const size_t i = Index(x, y, z);
        const uint8_t bit = static_cast<uint8_t>(1u << (i & 7u));
        m_Bits[i >> 3] = static_cast<uint8_t>(on ? (m_Bits[i >> 3] | bit) : (m_Bits[i >> 3] & ~bit));
    }

    bool VoxelMask::AnyInRange(size_t first, size_t last) const {
        last = std::min(last, VoxelCount());
        if (first >= last) return false;
        const size_t firstByte = first >> 3, lastByte = (last - 1) >> 3;
        const unsigned head = static_cast<unsigned>(0xFFu << (first & 7u)) & 0xFFu;
        const unsigned tail = 0xFFu >> (7u - ((last - 1) & 7u));
        if (firstByte == lastByte) return (m_Bits[firstByte] & head & tail) != 0;
        if ((m_Bits[firstByte] & head) != 0 || (m_Bits[lastByte] & tail) != 0) return true;
        for (size_t i = firstByte + 1; i < lastByte; ++i) {
            if (m_Bits[i] != 0) return true;
        }
        return false;
    }

    void VoxelMask::SetRange(size_t first, size_t last) {
        last = std::min(last, VoxelCount());
        for (size_t i = first; i < last;) {
            if ((i & 7u) == 0 && i + 8 <= last) {
                m_Bits[i >> 3] = 0xFFu;
                i += 8;
                continue;
            }
            m_Bits[i >> 3] = static_cast<uint8_t>(m_Bits[i >> 3] | (1u << (i & 7u)));
            ++i;
        }
    }

    uint32_t VoxelMask::Count() const {
        uint32_t n = 0;
        for (const uint8_t b : m_Bits) n += static_cast<uint32_t>(std::popcount(b));
        return n;
    }
}
