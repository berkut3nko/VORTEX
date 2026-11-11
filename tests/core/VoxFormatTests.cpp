// MagicaVoxel .vox import / export: byte-level format, axis convention, palette rule, scene
// graph translations and robustness against damaged files.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

import vortex.core;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {
    using Bytes = std::vector<uint8_t>;

    void PutI32(Bytes& out, int32_t value) {
        const uint32_t v = static_cast<uint32_t>(value);
        for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<uint8_t>((v >> shift) & 0xffu));
    }

    void PutString(Bytes& out, std::string_view text) {
        PutI32(out, static_cast<int32_t>(text.size()));
        out.insert(out.end(), text.begin(), text.end());
    }

    void PutDict(Bytes& out, std::initializer_list<std::pair<std::string_view, std::string_view>> entries) {
        PutI32(out, static_cast<int32_t>(entries.size()));
        for (const auto& [key, value] : entries) {
            PutString(out, key);
            PutString(out, value);
        }
    }

    Bytes MakeChunk(std::string_view id, const Bytes& content, const Bytes& children = {}) {
        Bytes out(id.begin(), id.end());
        PutI32(out, static_cast<int32_t>(content.size()));
        PutI32(out, static_cast<int32_t>(children.size()));
        out.insert(out.end(), content.begin(), content.end());
        out.insert(out.end(), children.begin(), children.end());
        return out;
    }

    void Append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

    Bytes SizeChunk(int x, int y, int z) {
        Bytes content;
        PutI32(content, x);
        PutI32(content, y);
        PutI32(content, z);
        return MakeChunk("SIZE", content);
    }

    /// `voxels` = x, y, z, colourIndex quadruples (MagicaVoxel axes).
    Bytes XyziChunk(const Bytes& voxels) {
        Bytes content;
        PutI32(content, static_cast<int32_t>(voxels.size() / 4));
        Append(content, voxels);
        return MakeChunk("XYZI", content);
    }

    /// Palette chunk whose entry e is (e, 255 - e, e / 2, 255).
    Bytes RgbaChunk() {
        Bytes content;
        for (int e = 0; e < 256; ++e) {
            content.push_back(static_cast<uint8_t>(e));
            content.push_back(static_cast<uint8_t>(255 - e));
            content.push_back(static_cast<uint8_t>(e / 2));
            content.push_back(255);
        }
        return MakeChunk("RGBA", content);
    }

    Bytes File(const Bytes& mainChildren) {
        Bytes out{'V', 'O', 'X', ' '};
        PutI32(out, 150);
        Append(out, MakeChunk("MAIN", {}, mainChildren));
        return out;
    }

    Bytes TransformNode(int id, int child, std::string_view name, std::string_view translation) {
        Bytes content;
        PutI32(content, id);
        if (name.empty()) PutDict(content, {});
        else PutDict(content, {{"_name", name}});
        PutI32(content, child);
        PutI32(content, -1);   // reserved
        PutI32(content, 0);    // layer
        PutI32(content, 1);    // frames
        if (translation.empty()) PutDict(content, {});
        else PutDict(content, {{"_t", translation}});
        return MakeChunk("nTRN", content);
    }

    Bytes GroupNode(int id, std::initializer_list<int> children) {
        Bytes content;
        PutI32(content, id);
        PutDict(content, {});
        PutI32(content, static_cast<int32_t>(children.size()));
        for (int child : children) PutI32(content, child);
        return MakeChunk("nGRP", content);
    }

    Bytes ShapeNode(int id, int model) {
        Bytes content;
        PutI32(content, id);
        PutDict(content, {});
        PutI32(content, 1);
        PutI32(content, model);
        PutDict(content, {});
        return MakeChunk("nSHP", content);
    }

    /// The smallest useful file: one 2 x 3 x 4 model with a single voxel at MagicaVoxel (1, 0, 3), colour index 7.
    Bytes MinimalFile() {
        Bytes children = SizeChunk(2, 3, 4);
        Append(children, XyziChunk({1, 0, 3, 7}));
        Append(children, RgbaChunk());
        return File(children);
    }
}

TEST(VoxFormat, MinimalFileAxisMappingAndPaletteRule) {
    const Bytes bytes = MinimalFile();
    std::string error;
    const std::optional<VoxFile> file = ParseVox(bytes, &error);
    ASSERT_TRUE(file.has_value()) << error;
    ASSERT_EQ(file->models.size(), 1u);

    // MagicaVoxel is Z-up: size (2, 3, 4) becomes engine (2, 4, 3).
    const VoxelVolume& volume = file->models[0].volume;
    EXPECT_EQ(volume.Size(), glm::ivec3(2, 4, 3));
    // (x, y, z) -> (x, z, sizeY - 1 - y) = (1, 3, 3 - 1 - 0).
    EXPECT_EQ(volume.CountSolid(), 1u);
    EXPECT_EQ(volume.Get(1, 3, 2), 7);
    EXPECT_EQ(file->models[0].translation, glm::ivec3(0));

    // Colour of voxel index i is chunk entry i - 1.
    EXPECT_EQ(file->palette[7], glm::u8vec4(6, 249, 3, 255));
    EXPECT_EQ(file->palette[1], glm::u8vec4(0, 255, 0, 255));
    EXPECT_EQ(file->palette[255], glm::u8vec4(254, 1, 127, 255));
}

TEST(VoxFormat, RoundTripPreservesVolumesAndPalette) {
    VoxFile source;
    for (size_t i = 1; i < source.palette.size(); ++i)
        source.palette[i] = glm::u8vec4(static_cast<uint8_t>(i), static_cast<uint8_t>(i * 7), static_cast<uint8_t>(255 - i), 255);

    VoxModel first;
    first.volume = VoxelVolume(glm::ivec3(5, 7, 3));
    for (int z = 0; z < 3; ++z)
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 5; ++x)
                if ((x + 2 * y + 3 * z) % 3 != 0) first.volume.Set(x, y, z, static_cast<uint8_t>(1 + (x * 31 + y * 17 + z * 5) % 255));
    VoxModel second;
    second.volume = VoxelVolume(glm::ivec3(1, 2, 9));
    second.volume.Set(0, 1, 8, 255);
    second.volume.Set(0, 0, 0, 1);
    source.models.push_back(first);
    source.models.push_back(second);

    const Bytes bytes = WriteVox(source);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "VOX ");
    EXPECT_EQ(bytes[4], 150);   // version, little endian
    EXPECT_EQ(bytes[5], 0);

    std::string error;
    const std::optional<VoxFile> parsed = ParseVox(bytes, &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    ASSERT_EQ(parsed->models.size(), source.models.size());
    for (size_t m = 0; m < source.models.size(); ++m) {
        EXPECT_EQ(parsed->models[m].volume.Size(), source.models[m].volume.Size());
        EXPECT_EQ(parsed->models[m].volume.Data(), source.models[m].volume.Data());
    }
    for (size_t i = 1; i < source.palette.size(); ++i) EXPECT_EQ(parsed->palette[i], source.palette[i]) << "entry " << i;
}

TEST(VoxFormat, SaveAndLoadFile) {
    VoxFile source;
    source.palette[3] = glm::u8vec4(10, 20, 30, 255);
    VoxModel model;
    model.volume = VoxelVolume(glm::ivec3(4));
    model.volume.FillBox(glm::ivec3(1), glm::ivec3(3), 3);
    source.models.push_back(model);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "vortex_voxformat_test.vox";
    ASSERT_TRUE(SaveVox(path, source));
    std::string error;
    const std::optional<VoxFile> loaded = LoadVox(path, &error);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    ASSERT_TRUE(loaded.has_value()) << error;
    ASSERT_EQ(loaded->models.size(), 1u);
    EXPECT_EQ(loaded->models[0].volume.Data(), model.volume.Data());
    EXPECT_EQ(loaded->palette[3], source.palette[3]);

    EXPECT_FALSE(LoadVox(std::filesystem::temp_directory_path() / "vortex_no_such_file.vox", &error).has_value());
    EXPECT_FALSE(error.empty());
    // A directory is not a file, even where it can be opened as a stream.
    error.clear();
    EXPECT_FALSE(LoadVox(std::filesystem::temp_directory_path(), &error).has_value());
    EXPECT_FALSE(error.empty());
}

TEST(VoxFormat, SceneGraphTranslationsAccumulateThroughGroups) {
    // root transform (1 2 3) -> group -> { transform "a" (10 20 30) -> shape(model 0),
    //                                      transform "b" (-5 0 4)   -> shape(model 1) }
    Bytes children = SizeChunk(1, 1, 1);
    Append(children, XyziChunk({0, 0, 0, 1}));
    Append(children, SizeChunk(1, 1, 1));
    Append(children, XyziChunk({0, 0, 0, 2}));
    Append(children, TransformNode(0, 1, "", "1 2 3"));
    Append(children, GroupNode(1, {2, 4}));
    Append(children, TransformNode(2, 3, "a", "10 20 30"));
    Append(children, ShapeNode(3, 0));
    Append(children, TransformNode(4, 5, "b", "-5 0 4"));
    Append(children, ShapeNode(5, 1));
    Append(children, RgbaChunk());

    std::string error;
    const std::optional<VoxFile> file = ParseVox(File(children), &error);
    ASSERT_TRUE(file.has_value()) << error;
    ASSERT_EQ(file->models.size(), 2u);
    // MagicaVoxel (tx, ty, tz) -> engine (tx, tz, -ty).
    EXPECT_EQ(file->models[0].translation, glm::ivec3(11, 33, -22));
    EXPECT_EQ(file->models[0].name, "a");
    EXPECT_EQ(file->models[1].translation, glm::ivec3(-4, 7, -2));
    EXPECT_EQ(file->models[1].name, "b");
}

TEST(VoxFormat, MissingPaletteGetsFallback) {
    Bytes children = SizeChunk(1, 1, 1);
    Append(children, XyziChunk({0, 0, 0, 1}));
    std::string error;
    const std::optional<VoxFile> file = ParseVox(File(children), &error);
    ASSERT_TRUE(file.has_value()) << error;
    // Every usable entry is opaque and the palette is not one flat colour.
    bool varied = false;
    for (size_t i = 1; i < 256; ++i) {
        EXPECT_EQ(file->palette[i].a, 255) << "entry " << i;
        varied = varied || file->palette[i] != file->palette[1];
    }
    EXPECT_TRUE(varied);
}

TEST(VoxFormat, VoxelsOutsideTheModelAreIgnored) {
    Bytes children = SizeChunk(2, 2, 2);
    Append(children, XyziChunk({0, 0, 0, 5, 2, 0, 0, 5, 0, 200, 0, 5, 1, 1, 1, 0}));
    Append(children, RgbaChunk());
    const std::optional<VoxFile> file = ParseVox(File(children));
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->models[0].volume.CountSolid(), 1u);
    EXPECT_EQ(file->models[0].volume.Get(0, 0, 1), 5);
}

TEST(VoxFormat, RejectsGarbage) {
    std::string error;
    EXPECT_FALSE(ParseVox({}, &error).has_value());
    EXPECT_FALSE(error.empty());

    const Bytes notVox{'R', 'I', 'F', 'F', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    error.clear();
    EXPECT_FALSE(ParseVox(notVox, &error).has_value());
    EXPECT_FALSE(error.empty());

    // Right magic, nonsense afterwards.
    Bytes noise{'V', 'O', 'X', ' '};
    uint32_t state = 12345;
    for (int i = 0; i < 512; ++i) {
        state = state * 1664525u + 1013904223u;
        noise.push_back(static_cast<uint8_t>(state >> 24));
    }
    EXPECT_FALSE(ParseVox(noise).has_value());

    // Header only, and a file without any model.
    EXPECT_FALSE(ParseVox(File({})).has_value());
    EXPECT_FALSE(ParseVox(File(RgbaChunk())).has_value());

    // XYZI without SIZE, and a voxel count larger than the chunk.
    EXPECT_FALSE(ParseVox(File(XyziChunk({0, 0, 0, 1}))).has_value());
    Bytes lying = SizeChunk(1, 1, 1);
    Bytes content;
    PutI32(content, 1000000);
    content.insert(content.end(), {0, 0, 0, 1});
    Append(lying, MakeChunk("XYZI", content));
    EXPECT_FALSE(ParseVox(File(lying)).has_value());

    // Absurd model sizes must not be allocated.
    Bytes huge = SizeChunk(0x7fffffff, 0x7fffffff, 0x7fffffff);
    Append(huge, XyziChunk({0, 0, 0, 1}));
    EXPECT_FALSE(ParseVox(File(huge)).has_value());
    // ... nor many individually acceptable models that add up to gigabytes (9 x 512^3 in under 400 bytes).
    Bytes many;
    for (int i = 0; i < 9; ++i) {
        Append(many, SizeChunk(512, 512, 512));
        Append(many, XyziChunk({}));
    }
    EXPECT_FALSE(ParseVox(File(many)).has_value());
    Bytes negative = SizeChunk(-4, 4, 4);
    Append(negative, XyziChunk({0, 0, 0, 1}));
    EXPECT_FALSE(ParseVox(File(negative)).has_value());
}

TEST(VoxFormat, EveryTruncationIsRejectedWithoutCrashing) {
    // Scene graph chunks included so their string / dictionary readers are exercised too.
    Bytes children = SizeChunk(2, 3, 4);
    Append(children, XyziChunk({1, 0, 3, 7}));
    Append(children, TransformNode(0, 1, "root", "1 2 3"));
    Append(children, GroupNode(1, {2}));
    Append(children, TransformNode(2, 3, "model", "4 5 6"));
    Append(children, ShapeNode(3, 0));
    Append(children, RgbaChunk());
    const Bytes bytes = File(children);
    ASSERT_TRUE(ParseVox(bytes).has_value());

    const LogLevel previous = Log::GetLevel();
    Log::SetLevel(LogLevel::Off);   // ~1500 expected errors
    size_t accepted = 0;
    for (size_t length = 0; length < bytes.size(); ++length)
        if (ParseVox(std::span<const uint8_t>(bytes.data(), length)).has_value()) ++accepted;
    Log::SetLevel(previous);
    EXPECT_EQ(accepted, 0u);
}

TEST(VoxFormat, CorruptedBytesNeverCrash) {
    const Bytes original = MinimalFile();
    const LogLevel previous = Log::GetLevel();
    Log::SetLevel(LogLevel::Off);
    uint32_t state = 987654321u;
    // Only the structural part (headers, SIZE, XYZI, first palette bytes) is worth corrupting.
    const size_t span = std::min<size_t>(original.size(), 96);
    for (int i = 0; i < 2000; ++i) {
        Bytes bytes = original;
        state = state * 1664525u + 1013904223u;
        const size_t position = (state >> 8) % span;
        state = state * 1664525u + 1013904223u;
        bytes[position] = static_cast<uint8_t>(state >> 24);
        const std::optional<VoxFile> file = ParseVox(bytes);
        if (file) {
            // Whatever was accepted must be internally consistent.
            for (const VoxModel& model : file->models) {
                const glm::ivec3 size = model.volume.Size();
                EXPECT_EQ(model.volume.Data().size(), static_cast<size_t>(size.x) * static_cast<size_t>(size.y) * static_cast<size_t>(size.z));
            }
        }
    }
    Log::SetLevel(previous);
}

TEST(VoxFormat, RegisterPaletteConvertsSrgbToLinear) {
    VoxFile file;
    file.palette[1] = glm::u8vec4(255, 0, 0, 255);
    file.palette[2] = glm::u8vec4(128, 128, 128, 255);
    file.palette[255] = glm::u8vec4(0, 0, 255, 255);

    MaterialLibrary library;
    Material prototype;
    prototype.roughness = 0.25f;
    prototype.density = 432.0f;
    const uint32_t before = library.PaletteCount();
    const PaletteId palette = RegisterVoxPalette(library, file, "robot", prototype);

    EXPECT_NE(palette, kWorldPalette);
    EXPECT_EQ(library.PaletteCount(), before + 1);
    EXPECT_EQ(library.PaletteName(palette), "robot");

    const Material& red = library.Get(palette, 1);
    EXPECT_NEAR(red.color.r, 1.0f, 1e-5f);
    EXPECT_NEAR(red.color.g, 0.0f, 1e-5f);
    EXPECT_NEAR(red.color.b, 0.0f, 1e-5f);
    EXPECT_FLOAT_EQ(red.roughness, 0.25f);
    EXPECT_FLOAT_EQ(red.density, 432.0f);

    // sRGB 128 is about 21.6 % linear, not 50 %.
    const Material& grey = library.Get(palette, 2);
    EXPECT_NEAR(grey.color.r, 0.2158f, 1e-3f);
    EXPECT_NEAR(grey.color.g, grey.color.r, 1e-6f);
    EXPECT_NEAR(grey.color.b, grey.color.r, 1e-6f);

    EXPECT_NEAR(library.Get(palette, 255).color.b, 1.0f, 1e-5f);
}
