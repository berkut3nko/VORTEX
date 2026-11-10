module;

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

module vortex.voxel;

import vortex.core;

namespace vortex::voxel {

    namespace {
        GpuMaterial Pack(const Material& m) {
            GpuMaterial g;
            g.colorEmissive = glm::vec4(m.color, m.emissive);
            g.params = glm::vec4(m.roughness, m.metallic, m.variation, 0.0f);
            return g;
        }

        /// sRGB (0..255) to linear RGB, so materials can be authored with familiar colour values.
        glm::vec3 Srgb(int r, int g, int b) {
            auto toLinear = [](int c) {
                const float v = static_cast<float>(c) / 255.0f;
                return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
            };
            return {toLinear(r), toLinear(g), toLinear(b)};
        }
    }

    MaterialLibrary::MaterialLibrary() { CreatePalette("world"); }

    PaletteId MaterialLibrary::CreatePalette(std::string_view name) {
        const PaletteId id = static_cast<PaletteId>(m_PaletteNames.size());
        m_PaletteNames.emplace_back(name);
        const size_t newSize = (static_cast<size_t>(id) + 1) * kPaletteSize;
        m_Materials.resize(newSize);
        m_Names.resize(newSize);
        m_Used.resize(newSize, 0);
        m_Gpu.resize(newSize);

        // Entry 0 is "empty" in every palette.
        const size_t base = static_cast<size_t>(id) * kPaletteSize;
        Material air;
        air.color = glm::vec3(0.0f);
        air.density = 0.0f;
        air.hardness = 0.0f;
        m_Materials[base] = air;
        m_Names[base] = "air";
        m_Used[base] = 1;
        m_Gpu[base] = Pack(air);
        // Unused entries render as a loud magenta so a bad voxel id is obvious.
        for (size_t i = 1; i < kPaletteSize; ++i) {
            m_Materials[base + i].color = glm::vec3(1.0f, 0.0f, 1.0f);
            m_Gpu[base + i] = Pack(m_Materials[base + i]);
        }
        ++m_Revision;
        return id;
    }

    std::string_view MaterialLibrary::PaletteName(PaletteId palette) const {
        return palette < m_PaletteNames.size() ? std::string_view(m_PaletteNames[palette]) : std::string_view{};
    }

    void MaterialLibrary::Set(PaletteId palette, uint8_t index, const Material& material, std::string_view name) {
        if (palette >= PaletteCount() || index == 0) {
            Log::Error("MaterialLibrary::Set: invalid palette {} / index {}", palette, static_cast<int>(index));
            return;
        }
        const size_t i = static_cast<size_t>(palette) * kPaletteSize + index;
        m_Materials[i] = material;
        m_Names[i] = std::string(name);
        m_Used[i] = 1;
        m_Gpu[i] = Pack(material);
        ++m_Revision;
    }

    uint8_t MaterialLibrary::Add(PaletteId palette, const Material& material, std::string_view name) {
        if (palette >= PaletteCount()) {
            Log::Error("MaterialLibrary::Add: invalid palette {}", palette);
            return 0;
        }
        const size_t base = static_cast<size_t>(palette) * kPaletteSize;
        for (uint32_t i = 1; i < kPaletteSize; ++i) {
            if (!m_Used[base + i]) {
                Set(palette, static_cast<uint8_t>(i), material, name);
                return static_cast<uint8_t>(i);
            }
        }
        Log::Error("MaterialLibrary::Add: palette '{}' is full (255 materials)", PaletteName(palette));
        return 0;
    }

    uint8_t MaterialLibrary::Find(PaletteId palette, std::string_view name) const {
        if (palette >= PaletteCount()) return 0;
        const size_t base = static_cast<size_t>(palette) * kPaletteSize;
        for (uint32_t i = 1; i < kPaletteSize; ++i) {
            if (m_Used[base + i] && m_Names[base + i] == name) return static_cast<uint8_t>(i);
        }
        return 0;
    }

    const Material& MaterialLibrary::Get(PaletteId palette, uint8_t index) const {
        static const Material fallback{};
        if (palette >= PaletteCount()) return fallback;
        return m_Materials[static_cast<size_t>(palette) * kPaletteSize + index];
    }

    std::string_view MaterialLibrary::Name(PaletteId palette, uint8_t index) const {
        if (palette >= PaletteCount()) return {};
        return m_Names[static_cast<size_t>(palette) * kPaletteSize + index];
    }

    WorldMaterials RegisterDefaultWorldMaterials(MaterialLibrary& lib) {
        WorldMaterials ids;
        auto add = [&lib](std::string_view name, glm::vec3 color, float roughness, float density, float hardness,
                          float variation = 0.08f, uint32_t flags = 0, float emissive = 0.0f, float metallic = 0.0f) {
            Material m;
            m.color = color;
            m.roughness = roughness;
            m.density = density;
            m.hardness = hardness;
            m.variation = variation;
            m.flags = flags;
            m.emissive = emissive;
            m.metallic = metallic;
            return lib.Add(kWorldPalette, m, name);
        };

        //                          name            colour                 rough  density  hard   var
        ids.grass        = add("grass",        Srgb(96, 148, 62),     0.95f, 1300.0f,  0.6f, 0.14f, MaterialFlag_Flammable);
        ids.grassDry     = add("grass_dry",    Srgb(150, 152, 74),    0.95f, 1300.0f,  0.6f, 0.14f, MaterialFlag_Flammable);
        ids.dirt         = add("dirt",         Srgb(112, 82, 56),     0.95f, 1500.0f,  0.8f, 0.12f);
        ids.stone        = add("stone",        Srgb(128, 128, 126),   0.85f, 2600.0f,  3.0f, 0.10f);
        ids.stoneDark    = add("stone_dark",   Srgb(84, 86, 92),      0.85f, 2800.0f,  4.0f, 0.10f);
        ids.sand         = add("sand",         Srgb(214, 196, 148),   0.90f, 1600.0f,  0.4f, 0.07f);
        ids.snow         = add("snow",         Srgb(236, 240, 246),   0.80f,  400.0f,  0.2f, 0.03f);
        ids.water        = add("water",        Srgb(44, 104, 160),    0.08f, 1000.0f,  0.0f, 0.02f, MaterialFlag_Liquid | MaterialFlag_Indestructible);
        ids.wood         = add("wood",         Srgb(104, 74, 46),     0.85f,  700.0f,  1.5f, 0.12f, MaterialFlag_Flammable);
        ids.leaves       = add("leaves",       Srgb(58, 120, 48),     0.90f,  200.0f,  0.2f, 0.20f, MaterialFlag_Flammable | MaterialFlag_Foliage);
        ids.leavesAutumn = add("leaves_autumn", Srgb(196, 112, 40),   0.90f,  200.0f,  0.2f, 0.20f, MaterialFlag_Flammable | MaterialFlag_Foliage);
        ids.planks       = add("planks",       Srgb(170, 130, 84),    0.80f,  600.0f,  1.2f, 0.08f, MaterialFlag_Flammable);
        ids.brick        = add("brick",        Srgb(158, 78, 62),     0.85f, 1900.0f,  2.5f, 0.12f);
        ids.iron         = add("iron",         Srgb(150, 152, 158),   0.35f, 7800.0f,  8.0f, 0.04f, 0, 0.0f, 1.0f);
        ids.gold         = add("gold",         Srgb(255, 200, 70),    0.25f, 19300.0f, 2.5f, 0.03f, 0, 0.0f, 1.0f);
        ids.crystal      = add("crystal",      Srgb(120, 210, 255),   0.15f, 2600.0f,  3.0f, 0.05f, 0, 4.0f);
        ids.lava         = add("lava",         Srgb(255, 110, 30),    0.60f, 3000.0f,  0.0f, 0.25f, MaterialFlag_Liquid | MaterialFlag_Indestructible, 6.0f);
        ids.torch        = add("torch",        Srgb(255, 190, 110),   0.60f,  500.0f,  0.3f, 0.10f, 0, 12.0f);
        ids.glass        = add("glass",        Srgb(200, 230, 235),   0.05f, 2500.0f,  0.5f, 0.01f);
        ids.bedrock      = add("bedrock",      Srgb(40, 40, 44),      0.90f, 3000.0f, 1.0e9f, 0.10f, MaterialFlag_Indestructible);
        return ids;
    }
}
