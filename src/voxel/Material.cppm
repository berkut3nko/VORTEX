module;

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module vortex.voxel:material;

export namespace vortex::voxel {

    /// @brief Bit flags describing how a material behaves.
    enum MaterialFlags : uint32_t {
        MaterialFlag_None           = 0,
        MaterialFlag_Indestructible = 1u << 0,   ///< Ignores carving / explosions (bedrock).
        MaterialFlag_Flammable      = 1u << 1,
        MaterialFlag_Liquid         = 1u << 2,   ///< Not a collider, rendered as a surface.
        MaterialFlag_Foliage        = 1u << 3,   ///< Soft: no debris bodies, only particles.
    };

    /**
     * @brief Everything the engine knows about one voxel material.
     * @details Rendering and gameplay properties live side by side on the CPU; the GPU only
     * receives the compact GpuMaterial below.
     */
    struct Material {
        // --- rendering ---
        glm::vec3 color{1.0f};        ///< Linear RGB albedo.
        float emissive = 0.0f;        ///< Emitted radiance = color * emissive.
        float roughness = 0.9f;
        float metallic = 0.0f;
        float variation = 0.08f;      ///< Per-voxel brightness jitter (0 = flat colour).

        // --- physics / gameplay ---
        float density = 1000.0f;      ///< kg / m^3.
        float friction = 0.6f;
        float restitution = 0.05f;
        float hardness = 1.0f;        ///< Resistance to carving; damage must exceed it.
        uint32_t flags = MaterialFlag_None;
    };

    /// @brief GPU material record (std430, 32 bytes). Mirrors `VxMaterial` in the shaders.
    struct GpuMaterial {
        glm::vec4 colorEmissive{0.0f};   ///< rgb = albedo, a = emissive strength.
        glm::vec4 params{0.0f};          ///< x = roughness, y = metallic, z = variation, w = unused.
    };
    static_assert(sizeof(GpuMaterial) == 32);

    using PaletteId = uint32_t;
    constexpr uint32_t kPaletteSize = 256;
    constexpr PaletteId kWorldPalette = 0;

    /**
     * @brief All material palettes of the game.
     * @details A voxel stores one byte: an index into the palette of the volume it belongs to
     * (0 is always "empty"). Palette 0 is the world palette used by terrain; imported models
     * usually bring their own palette (a MagicaVoxel file has 255 colours each), so the engine
     * supports any number of palettes instead of one global 255-material limit.
     *
     * On the GPU the palettes form one array: material index = palette * 256 + voxel id.
     */
    class MaterialLibrary {
    public:
        MaterialLibrary();

        /// @brief Adds an empty palette and returns its id.
        PaletteId CreatePalette(std::string_view name = {});
        uint32_t PaletteCount() const { return static_cast<uint32_t>(m_PaletteNames.size()); }
        std::string_view PaletteName(PaletteId palette) const;

        /// @brief Sets entry `index` (1..255) of a palette.
        void Set(PaletteId palette, uint8_t index, const Material& material, std::string_view name = {});
        /// @brief Adds a material to the first free entry. Returns 0 when the palette is full.
        uint8_t Add(PaletteId palette, const Material& material, std::string_view name = {});
        /// @brief Finds a material by name. Returns 0 if not found.
        uint8_t Find(PaletteId palette, std::string_view name) const;

        const Material& Get(PaletteId palette, uint8_t index) const;
        std::string_view Name(PaletteId palette, uint8_t index) const;

        static constexpr uint32_t GlobalIndex(PaletteId palette, uint8_t index) { return palette * kPaletteSize + index; }

        /// @brief Packed materials for the GPU, `PaletteCount() * 256` entries.
        std::span<const GpuMaterial> GpuData() const { return m_Gpu; }
        /// @brief Changes whenever GpuData() changes, so the renderer knows when to re-upload.
        uint64_t Revision() const { return m_Revision; }

    private:
        std::vector<Material> m_Materials;       // PaletteCount * 256
        std::vector<std::string> m_Names;        // PaletteCount * 256
        std::vector<uint8_t> m_Used;             // PaletteCount * 256
        std::vector<GpuMaterial> m_Gpu;          // PaletteCount * 256
        std::vector<std::string> m_PaletteNames;
        uint64_t m_Revision = 1;
    };

    /// @brief Voxel ids of the built-in world palette (see RegisterDefaultWorldMaterials).
    struct WorldMaterials {
        uint8_t grass = 0, grassDry = 0, dirt = 0, stone = 0, stoneDark = 0, sand = 0, snow = 0, water = 0;
        uint8_t wood = 0, leaves = 0, leavesAutumn = 0, planks = 0, brick = 0, iron = 0, gold = 0;
        uint8_t crystal = 0, lava = 0, torch = 0, glass = 0, bedrock = 0;
    };

    /// @brief Fills the world palette with a starter set of natural and building materials.
    WorldMaterials RegisterDefaultWorldMaterials(MaterialLibrary& library);
}
