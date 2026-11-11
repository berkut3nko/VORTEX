// Triangle voxeliser (overlap test, interior fill, limits) and glTF import.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

import vortex.core;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {
    /// Corner i of a box is (i & 1, i >> 1 & 1, i >> 2 & 1); two triangles per face.
    /// Face order: -X, +X, -Y, +Y, -Z, +Z.
    constexpr std::array<uint16_t, 36> kCubeIndices{0, 2, 6, 0, 6, 4, 1, 3, 7, 1, 7, 5, 0, 1, 5, 0, 5, 4,
                                                    2, 3, 7, 2, 7, 6, 0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6};

    glm::vec3 Corner(const glm::vec3& min, const glm::vec3& max, uint16_t i) {
        return {(i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z};
    }

    /// Closed axis-aligned box as 12 triangles. `faceMaterials` gives the id of each face in the
    /// order above; a face with id 0 is left out (open mesh).
    std::vector<MeshTriangle> BoxMesh(const glm::vec3& min, const glm::vec3& max, const std::array<uint8_t, 6>& faceMaterials = {1, 1, 1, 1, 1, 1}) {
        std::vector<MeshTriangle> triangles;
        for (size_t t = 0; t < 12; ++t) {
            const uint8_t material = faceMaterials[t / 2];
            if (material == 0) continue;
            MeshTriangle triangle;
            triangle.a = Corner(min, max, kCubeIndices[t * 3]);
            triangle.b = Corner(min, max, kCubeIndices[t * 3 + 1]);
            triangle.c = Corner(min, max, kCubeIndices[t * 3 + 2]);
            triangle.material = material;
            triangles.push_back(triangle);
        }
        return triangles;
    }

    bool OnShell(int x, int y, int z, int size) { return x == 0 || y == 0 || z == 0 || x == size - 1 || y == size - 1 || z == size - 1; }

    std::string Base64(const std::vector<uint8_t>& bytes) {
        constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < bytes.size(); i += 3) {
            const uint32_t b0 = bytes[i];
            const uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0u;
            const uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0u;
            const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
            out.push_back(alphabet[(triple >> 18) & 63u]);
            out.push_back(alphabet[(triple >> 12) & 63u]);
            out.push_back(i + 1 < bytes.size() ? alphabet[(triple >> 6) & 63u] : '=');
            out.push_back(i + 2 < bytes.size() ? alphabet[triple & 63u] : '=');
        }
        return out;
    }

    template <typename T>
    void PutRaw(std::vector<uint8_t>& out, size_t offset, T value) {
        std::memcpy(out.data() + offset, &value, sizeof(T));
    }

    /// Writes a .gltf (embedded buffer) holding a unit cube centred on the origin, built to
    /// exercise the awkward parts of the format:
    ///  - positions interleaved with padding (byteStride 16), behind a buffer view offset (4)
    ///    AND an accessor offset (16); every byte that is not a position is 0xFF garbage;
    ///  - 16-bit indices in a second view;
    ///  - the mesh sits under two nested nodes: parent scale 2, child translation (0.125, 0, 0)
    ///    -> world cube spans x in [-0.75, 1.25], y and z in [-1, 1];
    ///  - two materials, the primitive uses the second one (voxel id 2).
    std::filesystem::path WriteCubeGltf() {
        constexpr size_t kViewOffset = 4, kAccessorOffset = 16, kStride = 16;
        constexpr size_t kVertexBytes = kAccessorOffset + 8 * kStride;   // 144
        constexpr size_t kIndexOffset = kViewOffset + kVertexBytes;      // 148
        std::vector<uint8_t> buffer(kIndexOffset + kCubeIndices.size() * sizeof(uint16_t), 0xff);
        for (uint16_t i = 0; i < 8; ++i) {
            const glm::vec3 p = Corner(glm::vec3(-0.5f), glm::vec3(0.5f), i);
            const size_t offset = kViewOffset + kAccessorOffset + i * kStride;
            PutRaw(buffer, offset, p.x);
            PutRaw(buffer, offset + 4, p.y);
            PutRaw(buffer, offset + 8, p.z);
        }
        for (size_t i = 0; i < kCubeIndices.size(); ++i) PutRaw(buffer, kIndexOffset + i * sizeof(uint16_t), kCubeIndices[i]);

        std::string json = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"children": [1], "scale": [2.0, 2.0, 2.0]},
    {"mesh": 0, "translation": [0.125, 0.0, 0.0]},
    {"mesh": 0, "translation": [100.0, 0.0, 0.0]}
  ],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 1, "mode": 4}]}],
  "materials": [
    {"pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.0, 0.0, 1.0]}},
    {"pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.25, 1.0, 1.0]}}
  ],
  "accessors": [
    {"bufferView": 0, "byteOffset": 16, "componentType": 5126, "count": 8, "type": "VEC3", "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
    {"bufferView": 1, "componentType": 5123, "count": 36, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 4, "byteLength": 144, "byteStride": 16},
    {"buffer": 0, "byteOffset": 148, "byteLength": 72}
  ],
  "buffers": [{"byteLength": )";
        json += std::to_string(buffer.size());
        json += R"(, "uri": "data:application/octet-stream;base64,)";
        json += Base64(buffer);
        json += "\"}]\n}\n";

        const std::filesystem::path path = std::filesystem::temp_directory_path() / "vortex_meshimport_cube.gltf";
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << json;
        return path;
    }
}

TEST(Voxelize, ClosedCubeIsFilledSolid) {
    // Box (2.5 .. 7.5, 3.5 .. 8.5, -1.5 .. 3.5): bounds floor / ceil give a 6^3 volume at (2, 3, -2).
    const std::vector<MeshTriangle> mesh = BoxMesh(glm::vec3(2.5f, 3.5f, -1.5f), glm::vec3(7.5f, 8.5f, 3.5f), {4, 4, 4, 4, 4, 4});
    const VoxelizeResult result = VoxelizeTriangles(mesh, true);
    ASSERT_EQ(result.volume.Size(), glm::ivec3(6));
    EXPECT_EQ(result.origin, glm::vec3(2.0f, 3.0f, -2.0f));
    EXPECT_EQ(result.volume.Get(3, 3, 3), 4);
    EXPECT_EQ(result.volume.CountSolid(), 216u);
    for (uint8_t id : result.volume.Data()) ASSERT_EQ(id, 4);
}

TEST(Voxelize, ClosedCubeWithoutFillIsAHollowClosedShell) {
    const std::vector<MeshTriangle> mesh = BoxMesh(glm::vec3(0.5f), glm::vec3(5.5f));
    const VoxelizeResult result = VoxelizeTriangles(mesh, false);
    ASSERT_EQ(result.volume.Size(), glm::ivec3(6));
    EXPECT_EQ(result.volume.Get(3, 3, 3), 0);
    for (int z = 0; z < 6; ++z)
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 6; ++x) {
                // Closed: every shell voxel is solid. Thin: nothing behind the shell is.
                if (OnShell(x, y, z, 6)) EXPECT_NE(result.volume.Get(x, y, z), 0) << x << " " << y << " " << z;
                else EXPECT_EQ(result.volume.Get(x, y, z), 0) << x << " " << y << " " << z;
            }
}

TEST(Voxelize, InteriorTakesTheIdOfTheNearestSolidVoxelTowardsMinusX) {
    // -X face has id 3, everything else id 5. Interior rows start behind a -X face voxel.
    const std::vector<MeshTriangle> mesh = BoxMesh(glm::vec3(0.5f), glm::vec3(5.5f), {3, 5, 5, 5, 5, 5});
    const VoxelizeResult result = VoxelizeTriangles(mesh, true);
    ASSERT_EQ(result.volume.Size(), glm::ivec3(6));
    for (int z = 1; z < 5; ++z)
        for (int y = 1; y < 5; ++y) {
            ASSERT_EQ(result.volume.Get(0, y, z), 3);
            for (int x = 1; x < 5; ++x) EXPECT_EQ(result.volume.Get(x, y, z), 3) << x << " " << y << " " << z;
            EXPECT_EQ(result.volume.Get(5, y, z), 5);
        }
}

TEST(Voxelize, OpenMeshIsNotFilled) {
    // Without its +Y face the box is a bucket: the flood fill enters through the top.
    const std::vector<MeshTriangle> mesh = BoxMesh(glm::vec3(0.5f), glm::vec3(5.5f), {1, 1, 1, 0, 1, 1});
    const VoxelizeResult result = VoxelizeTriangles(mesh, true);
    ASSERT_EQ(result.volume.Size(), glm::ivec3(6));
    for (int z = 1; z < 5; ++z)
        for (int y = 1; y < 6; ++y)
            for (int x = 1; x < 5; ++x) EXPECT_EQ(result.volume.Get(x, y, z), 0) << x << " " << y << " " << z;
    EXPECT_NE(result.volume.Get(3, 0, 3), 0);   // the bottom is still there
}

TEST(Voxelize, TriangleCoversOnlyTheVoxelsItOverlaps) {
    // Right triangle in the plane z = 0.5 with its hypotenuse on x + y = 4.
    MeshTriangle triangle;
    triangle.a = glm::vec3(0.2f, 0.2f, 0.5f);
    triangle.b = glm::vec3(3.8f, 0.2f, 0.5f);
    triangle.c = glm::vec3(0.2f, 3.8f, 0.5f);
    triangle.material = 9;
    const VoxelizeResult result = VoxelizeTriangles(std::span<const MeshTriangle>(&triangle, 1), true);
    ASSERT_EQ(result.volume.Size(), glm::ivec3(4, 4, 1));
    EXPECT_EQ(result.origin, glm::vec3(0.0f));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            // Voxel [x, x + 1] x [y, y + 1] meets the triangle iff its minimum corner is inside
            // the hypotenuse; x + y == 4 only touches in a point and is left to the implementation.
            if (x + y < 4) EXPECT_EQ(result.volume.Get(x, y, 0), 9) << x << " " << y;
            if (x + y > 4) EXPECT_EQ(result.volume.Get(x, y, 0), 0) << x << " " << y;
        }
}

TEST(Voxelize, FirstWriterWins) {
    MeshTriangle first;
    first.a = glm::vec3(0.1f, 0.1f, 0.5f);
    first.b = glm::vec3(0.9f, 0.1f, 0.5f);
    first.c = glm::vec3(0.1f, 0.9f, 0.5f);
    first.material = 7;
    MeshTriangle second = first;
    second.material = 8;
    const std::array<MeshTriangle, 2> mesh{first, second};
    const VoxelizeResult result = VoxelizeTriangles(mesh, false);
    ASSERT_EQ(result.volume.Size(), glm::ivec3(1));
    EXPECT_EQ(result.volume.Get(0, 0, 0), 7);
}

TEST(Voxelize, EmptyInputGivesEmptyVolume) {
    const VoxelizeResult result = VoxelizeTriangles({}, true);
    EXPECT_EQ(result.volume.Size(), glm::ivec3(0));
    EXPECT_TRUE(result.volume.IsEmpty());
    EXPECT_TRUE(result.volume.Data().empty());

    // Triangles without finite coordinates count as no input.
    MeshTriangle broken;
    broken.a = glm::vec3(std::numeric_limits<float>::quiet_NaN());
    broken.b = glm::vec3(1.0f);
    broken.c = glm::vec3(std::numeric_limits<float>::infinity());
    const VoxelizeResult nothing = VoxelizeTriangles(std::span<const MeshTriangle>(&broken, 1), true);
    EXPECT_EQ(nothing.volume.Size(), glm::ivec3(0));
}

TEST(Voxelize, OversizedInputIsRejected) {
    const std::vector<MeshTriangle> mesh = BoxMesh(glm::vec3(0.5f), glm::vec3(40.5f, 5.5f, 5.5f));   // 41 x 6 x 6
    const VoxelizeResult rejected = VoxelizeTriangles(mesh, true, glm::ivec3(40, 64, 64));
    EXPECT_EQ(rejected.volume.Size(), glm::ivec3(0));
    EXPECT_TRUE(rejected.volume.IsEmpty());

    const VoxelizeResult accepted = VoxelizeTriangles(mesh, true, glm::ivec3(41, 6, 6));
    EXPECT_EQ(accepted.volume.Size(), glm::ivec3(41, 6, 6));
    EXPECT_EQ(accepted.volume.CountSolid(), 41u * 6u * 6u);

    // Coordinates far beyond any sane volume must be rejected, not overflow.
    const std::vector<MeshTriangle> galaxy = BoxMesh(glm::vec3(-1.0e30f), glm::vec3(1.0e30f));
    EXPECT_EQ(VoxelizeTriangles(galaxy, true).volume.Size(), glm::ivec3(0));
}

TEST(MeshImport, MissingFileFailsCleanly) {
    MeshImportSettings settings;
    settings.file = std::filesystem::temp_directory_path() / "vortex_no_such_model.glb";
    const MeshImportResult result = ImportMesh(settings);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
    EXPECT_TRUE(result.volume.IsEmpty());
}

TEST(MeshImport, RejectsBadSettingsAndBrokenFiles) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "vortex_meshimport_broken.glb";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "this is not a glTF binary";
    }
    MeshImportSettings settings;
    settings.file = path;
    const MeshImportResult broken = ImportMesh(settings);
    EXPECT_FALSE(broken.ok);
    EXPECT_FALSE(broken.error.empty());

    settings.voxelsPerUnit = 0.0f;
    const MeshImportResult noScale = ImportMesh(settings);
    EXPECT_FALSE(noScale.ok);
    EXPECT_FALSE(noScale.error.empty());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(MeshImport, GltfNodeTransformsStridesIndicesAndMaterials) {
    const std::filesystem::path path = WriteCubeGltf();
    MeshImportSettings settings;
    settings.file = path;
    settings.voxelsPerUnit = 4.0f;   // world cube of 2 units -> 8 voxels, all corners on integers
    settings.maxVoxelsPerAxis = 64;
    const MeshImportResult result = ImportMesh(settings);

    ASSERT_TRUE(result.ok) << result.error;
    // Node 2 (100 units away) is not part of the scene: including it would exceed 64 voxels.
    EXPECT_EQ(result.volume.Size(), glm::ivec3(8));
    EXPECT_EQ(result.volume.CountSolid(), 512u);
    for (uint8_t id : result.volume.Data()) ASSERT_EQ(id, 2);   // glTF material 1 -> voxel id 2

    ASSERT_EQ(result.materials.size(), 2u);
    EXPECT_NEAR(result.materials[0].color.r, 1.0f, 1e-5f);
    EXPECT_NEAR(result.materials[0].color.g, 0.0f, 1e-5f);
    EXPECT_NEAR(result.materials[1].color.r, 0.5f, 1e-5f);
    EXPECT_NEAR(result.materials[1].color.g, 0.25f, 1e-5f);
    EXPECT_NEAR(result.materials[1].color.b, 1.0f, 1e-5f);

    // Hollow import, and the size limit.
    settings.fillInterior = false;
    const MeshImportResult hollow = ImportMesh(settings);
    ASSERT_TRUE(hollow.ok) << hollow.error;
    EXPECT_EQ(hollow.volume.Get(4, 4, 4), 0);
    EXPECT_EQ(hollow.volume.CountSolid(), 512u - 216u);

    settings.maxVoxelsPerAxis = 7;
    const MeshImportResult tooBig = ImportMesh(settings);
    EXPECT_FALSE(tooBig.ok);
    EXPECT_FALSE(tooBig.error.empty());
    EXPECT_TRUE(tooBig.volume.IsEmpty());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(MeshImport, ImportsFrank) {
    const std::filesystem::path path = std::filesystem::path(VORTEX_TEST_SOURCE_DIR) / "models" / "frank.glb";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) GTEST_SKIP() << path.string() << " not present";

    MeshImportSettings settings;
    settings.file = path;
    settings.voxelsPerUnit = 6.0f;   // the model is a few units tall: a few dozen voxels
    settings.maxVoxelsPerAxis = 128;
    const MeshImportResult result = ImportMesh(settings);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.volume.IsEmpty());
    const glm::ivec3 size = result.volume.Size();
    EXPECT_GT(size.x, 1);
    EXPECT_GT(size.y, 1);
    EXPECT_GT(size.z, 1);
    EXPECT_LE(size.x, 128);
    EXPECT_LE(size.y, 128);
    EXPECT_LE(size.z, 128);

    // One textured material: its colour is the texture average, a valid linear colour.
    ASSERT_EQ(result.materials.size(), 1u);
    const glm::vec3 color = result.materials[0].color;
    for (int i = 0; i < 3; ++i) {
        EXPECT_GE(color[i], 0.0f);
        EXPECT_LE(color[i], 1.0f);
    }
    for (uint8_t id : result.volume.Data()) ASSERT_LE(id, 1);

    // Filling can only add voxels.
    settings.fillInterior = false;
    const MeshImportResult hollow = ImportMesh(settings);
    ASSERT_TRUE(hollow.ok) << hollow.error;
    EXPECT_EQ(hollow.volume.Size(), size);
    EXPECT_LE(hollow.volume.CountSolid(), result.volume.CountSolid());
}
