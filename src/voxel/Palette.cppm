module;

#include <vector>
#include <stdexcept>
#include <string>
#include <cstdint>
#include <glm/glm.hpp>

export module vortex.voxel:palette;

import :material;
import vortex.core; // For logging

namespace vortex::voxel {

    /**
     * @brief Manages a collection of materials indexed by voxel IDs.
     * @details Maximum 256 materials per palette (since Voxels are uint8_t).
     * Index 0 is reserved for "Empty/Air".
     */
    export class MaterialPalette {
    public:
        /**
         * @brief Initializes the palette with a default "Air" material at index 0.
         */
        MaterialPalette() {
            // Index 0 is always Air (Empty)
            PhysicalMaterial air{};
            air.color = glm::vec4(0.0f);
            air.density = 0.0f;
            air.hardness = 0.0f;
            m_Materials.push_back(air);
        }

        /**
         * @brief Adds a new material to the palette.
         * @param mat The material definition.
         * @return The index of the added material.
         * @warning Throws if palette size exceeds 255.
         */
        uint8_t AddMaterial(const PhysicalMaterial& mat) {
            if (m_Materials.size() >= 256) {
                Log::Error("MaterialPalette::AddMaterial: Palette full (max 256 materials)!");
                throw std::overflow_error("Palette full");
            }
            m_Materials.push_back(mat);
            return static_cast<uint8_t>(m_Materials.size() - 1);
        }

        /**
         * @brief Retrieves the raw data for GPU upload.
         */
        const std::vector<PhysicalMaterial>& GetData() const {
            return m_Materials;
        }

        /**
         * @brief Returns material by index (CPU side lookup).
         */
        const PhysicalMaterial& Get(uint8_t index) const {
            if (index >= m_Materials.size()) {
                // Return Error Pink on invalid index
                static PhysicalMaterial errorMat{}; 
                errorMat.color = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
                return errorMat;
            }
            return m_Materials[index];
        }

    private:
        std::vector<PhysicalMaterial> m_Materials;
    };
}