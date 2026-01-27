module;

#include <glm/glm.hpp>
#include <cstdint>

export module vortex.voxel:material;

namespace vortex::voxel {

    /**
     * @brief Bit flags defining material behavior.
     */
    export enum class MaterialFlags : uint32_t {
        None        = 0,
        Static      = 1 << 0, ///< Object cannot move (bedrock).
        Flammable   = 1 << 1, ///< Can catch fire.
        Conductive  = 1 << 2, ///< Conducts electricity.
        Liquid      = 1 << 3, ///< Behaves like fluid.
        Glowing     = 1 << 4  ///< Emits light (Self-illuminated).
    };

    /**
     * @brief Represents a single material definition for SHRED physics and rendering.
     * @details Aligned to 16 bytes for GPU Storage Buffer compatibility (std430 layout).
     * Total size: 64 bytes (cache-line friendly).
     */
    export struct alignas(16) PhysicalMaterial {
        // --- Visual Properties (16 bytes) ---
        /// @brief RGBA color. Alpha can be used for transparency or emission strength.
        glm::vec4 color;      

        // --- SHRED Physics Properties (16 bytes) ---
        /// @brief Mass per volume unit (affects physics simulation).
        float density;        
        /// @brief Surface friction coefficient [0..1].
        float friction;       
        /// @brief Bounciness/Elasticity [0..1].
        float restitution;    
        /// @brief Resistance to damage/penetration.
        float hardness;       

        // --- Structural Properties (16 bytes) ---
        /// @brief Max health before voxel destruction.
        float structuralHealth; 
        /// @brief How easily it catches fire.
        float flammability;     
        /// @brief Melting point or damage threshold from heat.
        float heatResistance;   
        /// @brief Bitmask of MaterialFlags.
        uint32_t flags;         

        // --- Padding/Reserved (16 bytes) ---
        // Reserved for future use (texture indices, sound IDs, etc.)
        // Keeping struct size power-of-two is good for GPU indexing.
        uint32_t textureIndex; 
        uint32_t soundImpactID;
        uint32_t soundDestroyID;
        uint32_t _pad0;
    };

    /**
     * @brief Helper to combine flags bitwise.
     */
    export inline constexpr uint32_t operator|(MaterialFlags a, MaterialFlags b) {
        return static_cast<uint32_t>(a) | static_cast<uint32_t>(b);
    }
}