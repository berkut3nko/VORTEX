module;

#include <vector>
#include <string>
#include <cstdint>

export module vortex.graphics:shader;

namespace vortex::graphics {

    /**
     * @brief Enum representing the programmable shader stage.
     */
    enum class ShaderStage {
        Vertex,
        Fragment,
        Compute,
        RayGen,
        Miss,
        ClosestHit
    };

    /**
     * @brief Helper class to compile GLSL code to SPIR-V at runtime using glslang.
     */
    export class ShaderCompiler {
    public:
        /**
         * @brief Initializes the glslang library.
         */
        static void Init();
        
        /**
         * @brief Finalizes the glslang library.
         */
        static void Shutdown();

        /**
         * @brief Compiles GLSL source code into SPIR-V binary.
         * @param stage The shader stage (Compute, Vertex, etc.).
         * @param source The GLSL source code string.
         * @return A vector of uint32_t containing SPIR-V bytecode.
         * @warning Throws std::runtime_error if compilation or linking fails.
         */
        static std::vector<uint32_t> Compile(ShaderStage stage, const std::string& source);
    };

}