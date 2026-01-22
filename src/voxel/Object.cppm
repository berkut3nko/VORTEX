module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

export module vortex.voxel:object;

import :chunk;

namespace vortex::voxel {

    export struct VoxelObject {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};

        std::shared_ptr<Chunk> chunk;
        bool isStatic = false;
        
        glm::mat4 GetTransformMatrix() const {
            glm::mat4 mat = glm::mat4(1.0f);
            mat = glm::translate(mat, position);
            mat = mat * glm::mat4_cast(rotation);
            mat = glm::scale(mat, scale);
            return mat;
        }
    };
}