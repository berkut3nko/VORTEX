module;

#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

export module vortex.voxel:beam;

namespace vortex::voxel {

    /**
     * @brief Represents a packet of rays (Beam) optimized for traversal.
     * @details Used in Beam Optimization to skip empty space for a group of rays.
     */
    export struct BeamPacket {
        glm::vec3 origin;
        glm::vec3 direction; // Main direction of the beam
        float minT;          // Min distance
        float maxT;          // Max distance
        
        // Frustum planes for the beam (simplified)
        glm::vec4 planes[4]; 
        
        // Active lanes bitmask (e.g., for 8x8 beam, 64 bits)
        uint64_t activeMask;
    };

    /**
     * @brief Manages the beam optimization data and logic.
     */
    export class BeamOptimizer {
    public:
        BeamOptimizer();
        
        /**
         * @brief Prepares beams for the current camera view.
         * @param viewProj Inverse View-Projection matrix to unproject rays.
         * @param width Screen width.
         * @param height Screen height.
         */
        void UpdateBeams(const glm::mat4& viewProj, uint32_t width, uint32_t height);

        /**
         * @brief Returns raw data for upload to GPU buffer.
         */
        const std::vector<BeamPacket>& GetBeamData() const { return m_Beams; }

    private:
        std::vector<BeamPacket> m_Beams;
        
        /**
         * @brief Group rays into coherency packets.
         */
        void GeneratePackets(uint32_t width, uint32_t height);
    };

}