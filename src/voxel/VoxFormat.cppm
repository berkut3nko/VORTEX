module;

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module vortex.voxel:voxformat;

import :volume;
import :material;

export namespace vortex::voxel {

    /// @brief One model of a MagicaVoxel file, converted to the engine's Y-up convention.
    struct VoxModel {
        VoxelVolume volume;
        glm::ivec3 translation{0};      ///< Scene-graph translation of the model centre (engine axes), 0 if absent.
        std::string name;
    };

    /**
     * @brief Contents of a MagicaVoxel `.vox` file.
     * @details Axes: MagicaVoxel is Z-up; on import (x, y, z) becomes engine (x, z, sizeY - 1 - y),
     * which keeps the model right-handed and upright. Voxel ids are the file's colour indices
     * (1..255) unchanged; `palette[i]` is the sRGB colour of id `i` (entry 0 is unused).
     */
    struct VoxFile {
        std::vector<VoxModel> models;
        std::array<glm::u8vec4, 256> palette{};
    };

    /// @brief Parses a `.vox` file from memory. On failure returns nullopt and fills `error`.
    std::optional<VoxFile> ParseVox(std::span<const uint8_t> bytes, std::string* error = nullptr);
    std::optional<VoxFile> LoadVox(const std::filesystem::path& path, std::string* error = nullptr);

    /// @brief Serialises models + palette as a `.vox` file (version 150, SIZE/XYZI/RGBA chunks).
    std::vector<uint8_t> WriteVox(const VoxFile& file);
    bool SaveVox(const std::filesystem::path& path, const VoxFile& file);

    /**
     * @brief Registers the file's palette as a new material palette (sRGB -> linear albedo) and
     * returns its id. Use it as the palette of VoxelModels created from the file's volumes.
     * @param prototype Non-colour properties (roughness, density ...) copied into every entry.
     */
    PaletteId RegisterVoxPalette(MaterialLibrary& library, const VoxFile& file, std::string_view name,
                                 const Material& prototype = Material{});
}
