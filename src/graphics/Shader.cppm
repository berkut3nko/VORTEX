module;

#include <vector>
#include <string>
#include <cstdint>

export module vortex.graphics:shader;

namespace vortex::graphics {

    enum class ShaderStage {
        Vertex,
        Fragment,
        Compute,
        RayGen,
        Miss,
        ClosestHit
    };

    /**
     * @brief Compiles GLSL code to SPIR-V at runtime.
     */
    export class ShaderCompiler {
    public:
        static void Init();
        static void Shutdown();

        /**
         * @brief Compiles GLSL source code into SPIR-V binary.
         * @param stage The shader stage (Compute, Vertex, etc.).
         * @param source The GLSL source code string.
         * @return A vector of uint32_t containing SPIR-V bytecode.
         */
        static std::vector<uint32_t> Compile(ShaderStage stage, const std::string& source);
    };

}