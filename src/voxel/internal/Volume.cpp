module;

#include <glm/glm.hpp>

#include <algorithm>
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
}
