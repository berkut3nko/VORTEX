module;

#include <vector>
#include <glm/glm.hpp>

export module vortex.physics:collider_builder;

import vortex.voxel;

namespace vortex::physics {

    /**
     * @brief Represents a simplified Axis-Aligned Bounding Box for physics generation.
     */
    export struct ColliderBox {
        glm::vec3 min;
        glm::vec3 size;
        uint8_t materialID; // To set friction/restitution based on material
    };

    /**
     * @brief Responsible for converting Voxel Chunks into optimized physics shapes.
     * @details Uses a Greedy Meshing approach tailored for physics boxes (not rendering quads).
     * This significantly reduces the number of child shapes in a Jolt CompoundShape.
     */
    export class VoxelColliderBuilder {
    public:
        /**
         * @brief Generates a list of optimized boxes from a chunk.
         * @param chunk The source voxel data.
         * @return A vector of boxes representing the solid volume.
         */
        static std::vector<ColliderBox> Build(const vortex::voxel::Chunk& chunk);
    };
}