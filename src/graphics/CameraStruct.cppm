module;

#include <glm/glm.hpp>

export module vortex.graphics:camera_struct;

namespace vortex::graphics {
    export struct Camera {
        glm::vec3 position{0.0f, 0.0f, 5.0f};
        glm::vec3 front{0.0f, 0.0f, -1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        float yaw = -90.0f; 
        float pitch = 0.0f; 
        float fov = 60.0f;
    };
}