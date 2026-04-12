module;

#include <glm/glm.hpp>

export module vortex.graphics:camera_struct;

namespace vortex::graphics {
    /**
     * @brief Represents the state of a 3D Camera.
     */
    export struct Camera {
        glm::vec3 position{0.0f, 0.0f, 5.0f};
        glm::vec3 front{0.0f, 0.0f, -1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        float yaw = -90.0f; 
        float pitch = 0.0f; 
        float fov = 60.0f;
    };

    /**
     * @brief GPU Camera Buffer (Uniform Buffer Object).
     * @note Must MATCH the std140 layout in shaders/voxel.vert EXACTLY.
     */
    export struct CameraUBO {
        // --- Shader Accessible Fields (Order matters!) ---
        glm::mat4 viewInverse; // Offset 0
        glm::mat4 projInverse; // Offset 64
        
        glm::vec4 position;    // Offset 128
        glm::vec4 direction;   // Offset 144
        
        uint32_t objectCount;  // Offset 160
        float _pad[3];         // Padding to align next mat4 to 16 bytes (160+4+12 = 176)
        
        glm::mat4 viewProj;    // Offset 176
        
        // --- Extra/Internal Fields (Appended at the end) ---
        glm::mat4 prevViewProj; 
        glm::mat4 view;
        glm::mat4 proj;
        glm::vec4 jitter;
    };
}