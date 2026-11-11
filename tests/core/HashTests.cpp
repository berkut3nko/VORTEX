// Determinism floor: content hashes of chunks and worlds, and the golden terrain test that
// pins the built-in generator bit for bit (docs/design/networking.md, stage 0).

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;

using namespace vortex;
using namespace vortex::voxel;

namespace {
    constexpr int kGroundHeight = 40;      // flat world: voxels with y < 40 are solid
    constexpr uint8_t kTopId = 2;
    constexpr uint8_t kRockId = 3;

    /// A small flat world, fully loaded: chunk layer y = 1 is dense, below uniform rock, above air.
    struct HashWorld {
        explicit HashWorld(int threads = 3) : store(512), jobs(static_cast<uint32_t>(threads)) {
            WorldConfig config;
            config.voxelSize = 0.5f;
            config.windowSize = {8, 8, 8};
            world = std::make_unique<World>(config, store, jobs, CreateFlatGenerator(kGroundHeight, kTopId, kRockId));
            world->LoadBlocking(glm::dvec3(0.0, 20.0, 0.0));
        }
        glm::ivec3 Min() const { return world->WindowMinChunk(); }
        glm::ivec3 Max() const { return world->WindowMinChunk() + world->WindowSize(); }
        ChunkStore store;
        JobSystem jobs;
        std::unique_ptr<World> world;
    };

    // --- golden terrain ---------------------------------------------------------------------
    constexpr uint32_t kGoldenSeeds[3] = {1337u, 77u, 0x9e3779b9u};
    constexpr int kGoldenChunks = 64;

    struct ProbeCoord {
        int x, y, z;
    };

    // The 64 probe chunks of each seed, chosen once by looking at the generated heights (default
    // settings, 3.2 m chunks) so that they cut what the generator can produce:
    //   0-15   the surface chunk of 16 columns within +-130 m (beach, sea, grass, hills)
    //   16-31  the same columns at y = -3 (caves, dark stone)
    //   32-39  one chunk above the surface of the first 8 columns (tree tops, the air cut-off)
    //          (seed 0x9e3779b9: #32 is (-4, 1, -6), a chunk with a tree trunk, which the others lacked)
    //   40-47  four columns 43-52 m high, surface chunk and the one below (snow, mountains)
    //   48-55  the surface chunk of 8 columns 1 km out (float precision far from the origin)
    //   56-63  those far columns underground
    // The coordinates are fixed: they are never derived from the build under test.
    constexpr ProbeCoord kGoldenCoords[3][kGoldenChunks] = {
        {   // seed 1337
            {-41, 0, -29},     {-6, -1, -29},     {0, -1, -29},      {23, -1, -29},     {-41, 0, -1},      {-6, 0, -1},
            {0, 1, -1},        {23, 0, -1},       {-41, 0, 5},       {-6, 4, 5},        {0, 5, 5},         {23, 0, 5},
            {-41, 0, 37},      {-6, 6, 37},       {0, 3, 37},        {23, 3, 37},       {-41, -3, -29},    {-6, -3, -29},
            {0, -3, -29},      {23, -3, -29},     {-41, -3, -1},     {-6, -3, -1},      {0, -3, -1},       {23, -3, -1},
            {-41, -3, 5},      {-6, -3, 5},       {0, -3, 5},        {23, -3, 5},       {-41, -3, 37},     {-6, -3, 37},
            {0, -3, 37},       {23, -3, 37},      {-41, 1, -29},     {-6, 0, -29},      {0, 0, -29},       {23, 0, -29},
            {-41, 1, -1},      {-6, 1, -1},       {0, 2, -1},        {23, 1, -1},       {0, 16, 18},       {0, 15, 18},
            {-168, 16, 99},    {-168, 15, 99},    {117, 15, -258},   {117, 14, -258},   {219, 15, -192},   {219, 14, -192},
            {-320, 0, -320},   {320, -3, -320},   {-320, -1, 320},   {320, 3, 320},     {-320, 1, 0},      {320, 11, 0},
            {0, 2, -320},      {0, 0, 320},       {-320, -3, -320},  {320, -2, -320},   {-320, -3, 320},   {320, -3, 320},
            {-320, -3, 0},     {320, -3, 0},      {0, -3, -320},     {0, -3, 320},
        },
        {   // seed 77
            {-41, 3, -29},     {-6, 1, -29},      {0, 1, -29},       {23, 0, -29},      {-41, 0, -1},      {-6, 3, -1},
            {0, 4, -1},        {23, -1, -1},      {-41, 2, 5},       {-6, 4, 5},        {0, 2, 5},         {23, -1, 5},
            {-41, 1, 37},      {-6, 0, 37},       {0, 0, 37},        {23, -1, 37},      {-41, -3, -29},    {-6, -3, -29},
            {0, -3, -29},      {23, -3, -29},     {-41, -3, -1},     {-6, -3, -1},      {0, -3, -1},       {23, -3, -1},
            {-41, -3, 5},      {-6, -3, 5},       {0, -3, 5},        {23, -3, 5},       {-41, -3, 37},     {-6, -3, 37},
            {0, -3, 37},       {23, -3, 37},      {-41, 4, -29},     {-6, 2, -29},      {0, 2, -29},       {23, 1, -29},
            {-41, 1, -1},      {-6, 4, -1},       {0, 5, -1},        {23, 0, -1},       {57, 16, -177},    {57, 15, -177},
            {-126, 15, -168},  {-126, 14, -168},  {-153, 15, 312},   {-153, 14, 312},   {-78, 15, 222},    {-78, 14, 222},
            {-320, -2, -320},  {320, 1, -320},    {-320, -1, 320},   {320, 2, 320},     {-320, 0, 0},      {320, 5, 0},
            {0, 3, -320},      {0, 0, 320},       {-320, -3, -320},  {320, -3, -320},   {-320, -3, 320},   {320, -3, 320},
            {-320, -3, 0},     {320, -3, 0},      {0, -3, -320},     {0, -3, 320},
        },
        {   // seed 2654435769
            {-41, -1, -29},    {-6, 1, -29},      {0, 0, -29},       {23, 2, -29},      {-41, -2, -1},     {-6, 0, -1},
            {0, 1, -1},        {23, 3, -1},       {-41, -1, 5},      {-6, 0, 5},        {0, 1, 5},         {23, 6, 5},
            {-41, -1, 37},     {-6, 1, 37},       {0, 0, 37},        {23, 0, 37},       {-41, -3, -29},    {-6, -3, -29},
            {0, -3, -29},      {23, -3, -29},     {-41, -3, -1},     {-6, -3, -1},      {0, -3, -1},       {23, -3, -1},
            {-41, -3, 5},      {-6, -3, 5},       {0, -3, 5},        {23, -3, 5},       {-41, -3, 37},     {-6, -3, 37},
            {0, -3, 37},       {23, -3, 37},      {-4, 1, -6},       {-6, 2, -29},      {0, 1, -29},       {23, 3, -29},
            {-41, -1, -1},     {-6, 1, -1},       {0, 2, -1},        {23, 4, -1},       {-177, 15, -177},  {-177, 14, -177},
            {252, 15, 249},    {252, 14, 249},    {-66, 15, 165},    {-66, 14, 165},    {51, 14, 99},      {51, 13, 99},
            {-320, -1, -320},  {320, 0, -320},    {-320, -1, 320},   {320, 6, 320},     {-320, 0, 0},      {320, 1, 0},
            {0, 1, -320},      {0, 0, 320},       {-320, -3, -320},  {320, -3, -320},   {-320, -3, 320},   {320, -3, 320},
            {-320, -3, 0},     {320, -3, 0},      {0, -3, -320},     {0, -3, 320},
        },
    };

    // Generated by this test on Linux x86-64 (Clang, -mavx2 -mfma -ffp-contract=off). A mismatch
    // prints the complete actual table; a generator change on purpose replaces these values, any
    // other mismatch means two builds no longer produce the same terrain.
    constexpr uint64_t kGoldenHashes[3][kGoldenChunks] = {
        {   // seed 1337
            0x7013b7ab83d851cdull, 0x5cc8b082130b58d6ull, 0x99ccace628a99d13ull, 0xab71c59e842c055bull,
            0xbe30fccb54e3991eull, 0x0104f450822bee34ull, 0xe8a164604889c5f4ull, 0xdc0cdab28158fd45ull,
            0x447d3989b9c557eaull, 0x3ee58941ef1c36fcull, 0x8a1f969f08cbc8a0ull, 0xcaaa39c02dfd1006ull,
            0x3e9f3f96abf540f2ull, 0xa661e7b8b1aeee9dull, 0xa02efae6879b7e39ull, 0x6a5d87eae799c80dull,
            0xcd3bcbd66e36e502ull, 0x5c8b5121855c2eb2ull, 0x9460a33b0da4bd6eull, 0xe43fdd698869124cull,
            0x342a000c85a0783dull, 0xcfb531d7c82d673full, 0x732b2b13a600474cull, 0x43676c3de66b154cull,
            0xec502bced690f0f5ull, 0x9b09bca5d835a0ddull, 0x145cbdb90bf57c0aull, 0xb0ae535786d5ce8cull,
            0x0dcc4e8ba0345afeull, 0x285781b90d6a2624ull, 0x1d57ed3aa92bd085ull, 0x2d85ad189b8efdd7ull,
            0xc90a5794385e8593ull, 0x304bb2631647de62ull, 0x304bb2631647de62ull, 0x304bb2631647de62ull,
            0xc8b46c83f20d455eull, 0x0fd4b4f4c217a96cull, 0xf2d0491a6912d5a8ull, 0x304bb2631647de62ull,
            0xd00dc2543b649298ull, 0x654699f68f4fc5acull, 0xdcae96f9a422fe98ull, 0x33430a485616933eull,
            0xfad30a970f72b782ull, 0xcd464cf59e925a21ull, 0xa5e8870cb9dffa7bull, 0xdeae16858ca76fa4ull,
            0x260527f6348f8ffaull, 0xa23ab0219cb4c89aull, 0xe6f978ef12e11b20ull, 0xc75dd3432ca1f5ffull,
            0x53c294a683c83a44ull, 0x244d6031e337d023ull, 0x9550011f699c9d82ull, 0xd3d1143ba6f446d9ull,
            0xdeef1954fa81ecfeull, 0x934755f40b775cd3ull, 0xa12192a9ffed8439ull, 0x1657217acf07c148ull,
            0x7ef5e862f31ceda1ull, 0x72664e589094e0a4ull, 0x389c9244519ae616ull, 0x16d9321542ce589bull,
        },
        {   // seed 77
            0x248f6ca15256ab89ull, 0x5f090e2984103b17ull, 0x74f7a4088e1de46eull, 0x69f8827a77f9dda7ull,
            0x2e37d4d589c53eb8ull, 0xe4561bf831a33478ull, 0x5b62b50f334a43e9ull, 0x1017c6d6b1adbef9ull,
            0xcbd1a569960ae1e3ull, 0x7bd01158149b8886ull, 0x04b0ff51f1935b2bull, 0x4357e76d6c6193eaull,
            0x0f2994d18ad67467ull, 0x27c1ea37a965b6beull, 0xdc42996b32b60786ull, 0x156bfc931c636bbaull,
            0x90f5a302ec9a492bull, 0x30571d0f2e39b58bull, 0xbbec462dbba76d8eull, 0xa12192a9ffed8439ull,
            0x8db90ab45d36345eull, 0x7ed0f2bafb0ada61ull, 0xbcf1a95a3ca1f253ull, 0xb6f1365b32b58116ull,
            0xbffe0b7e85acb86aull, 0x87fbcee5451f1e0bull, 0xd954691e95fee51cull, 0x5b6626ccc81b1f45ull,
            0xf7e911811c91927cull, 0x6e7b4b79f0bd82d3ull, 0x415ae785779e7839ull, 0xe97544d39af73b2dull,
            0x304bb2631647de62ull, 0xa285a040b2a8abebull, 0x304bb2631647de62ull, 0x5350dce09615b12dull,
            0x12dc9f64dfd7ed19ull, 0x2e2b381ea916188eull, 0x304bb2631647de62ull, 0x304bb2631647de62ull,
            0x2436480974db96e7ull, 0x644e5cbf00c83a9full, 0x588be8cbaf52da6aull, 0x428cebc7f9934240ull,
            0x7754734f6c2ab58cull, 0x9162efc402118144ull, 0x1e84170509ea6106ull, 0x7f77dfb6182fe8a6ull,
            0x5dce43f3a767f085ull, 0xb4b96994bd600a18ull, 0x09ac8a72a40e6d66ull, 0x5edf1fc75499dcf8ull,
            0x9fc21526937fe17aull, 0x126cc2fdb6d54099ull, 0xf6398740ddedaf42ull, 0xbfa058772cebf3c7ull,
            0x929a7bfce9cda8a0ull, 0x631130b67fc2f3c8ull, 0x2d0525641206e47full, 0x72664e589094e0a4ull,
            0x71c3b9443f914c3cull, 0x80c454c5a447dd5cull, 0x033ac388ee2ed214ull, 0xf830ed21c6b55ef1ull,
        },
        {   // seed 2654435769
            0x41def3401ed530a7ull, 0xd45c116e7f181fd7ull, 0x5763678dbe880a0eull, 0x5f2ff7f46689a5c7ull,
            0xc5aed1f3cbcfa671ull, 0xdfd2e821954fd61dull, 0x9cf539ff1fa04820ull, 0x6cb8dfc07fa1d3a5ull,
            0xa1cbb914f44557adull, 0x30890faf39a9db28ull, 0xe2e31ef21cdbdb90ull, 0xa3e23fd319a6945bull,
            0x979d3fb2c553890bull, 0xc88eab08090b5aa3ull, 0x255b14dcdf4bbf9bull, 0x7b98f0b491c733d8ull,
            0x9e369f8542db3069ull, 0x497adafe04b8a1c0ull, 0xea46211efdae067dull, 0x3e3d212d155b5899ull,
            0x5536c04584d826baull, 0x90a92c8602db4736ull, 0xb2d2d22ea7d807f1ull, 0x9878c6d4e88e2117ull,
            0x8716f07650caa105ull, 0xc6968d0a7ea92c33ull, 0x86b05c05a5e5a543ull, 0x6accedebe204c1d6ull,
            0x1c7f03c905451162ull, 0x0f7906a4e0c2557full, 0xa12192a9ffed8439ull, 0x3bb9eb66c8112d84ull,
            0x37a7fdc16c9431fbull, 0x304bb2631647de62ull, 0x304bb2631647de62ull, 0x304bb2631647de62ull,
            0x8eee65542a161aecull, 0x7ce30eac7b8b055aull, 0x304bb2631647de62ull, 0x76809f0b7f8df676ull,
            0xc74a4a6894d17639ull, 0x6dddfb74926cefbaull, 0x04169328adbbb5fdull, 0x024e62930ead7aa3ull,
            0xe7b4eef7143d1696ull, 0x48c9a652a95cd23bull, 0x675ed5dd134f6b6aull, 0x7325e873447b92a3ull,
            0xca3f25e575fc2055ull, 0x7dcf756640dbf452ull, 0x49832476905559daull, 0xd471c15e7dfa08b6ull,
            0x024537bcbb050e32ull, 0x419c0d88cd96031dull, 0xc1955a3f05d10f1bull, 0x686228536c3ac2b3ull,
            0x5a39100986e6a173ull, 0x700f9cfe0dd7b13full, 0xe747bef1b930bc4aull, 0x71694e41c3405e46ull,
            0x27a97e5a5768e955ull, 0xb17819521e79c10aull, 0x492782380d098123ull, 0x7cdee5ab32e66f49ull,
        },
    };

    // --- golden float bits ------------------------------------------------------------------
    // The chunk hashes above see floats only after floor() and threshold compares: a last-bit
    // difference moves a voxel in roughly one column of 1e5, so a build that rounds differently
    // can pass them. These tests compare the raw bit patterns of the noise and of the terrain
    // height instead, where every such difference shows.
    constexpr int kFloatProbes = 4096;
    constexpr int kNoiseChannels = 7;
    constexpr const char* kNoiseChannelNames[kNoiseChannels] = {
        "Sample(x/55, z/55)", "Sample(x/9, y/6, z/9)", "Fbm(/420, 3)", "Fbm(/95, 4)", "Ridged(/300, 5)", "Fbm(/11, 2)",
        "SmoothStep(0.05, 0.55, Fbm(/420, 3))"};

    constexpr uint64_t AbsorbBits(uint64_t h, uint32_t bits) {
        uint64_t x = h + 0x9e3779b97f4a7c15ull + bits;
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebull;
        x ^= x >> 31;
        return x;
    }

    /// Probe coordinate `k` of stream `salt` in metres: a multiple of 0.1 m within +-range.
    float ProbeMetres(int k, int salt, int rangeDecimetres) {
        const uint32_t h = vortex::Hash3(k, salt, 0, 0x5eedu);
        return static_cast<float>(static_cast<int>(h % static_cast<uint32_t>(2 * rangeDecimetres + 1)) - rangeDecimetres) * 0.1f;
    }

    /// Hash of the bit patterns of one noise function over the probe points (out to 1 km, the
    /// argument scales the generator uses).
    uint64_t NoiseBits(uint32_t seed, int channel) {
        const math::SimplexNoise noise(seed);
        uint64_t h = 0;
        for (int k = 0; k < kFloatProbes; ++k) {
            const float x = ProbeMetres(k, 1, 10000), z = ProbeMetres(k, 2, 10000), y = ProbeMetres(k, 3, 400);
            float v = 0.0f;
            switch (channel) {
                case 0: v = noise.Sample(x / 55.0f, z / 55.0f); break;
                case 1: v = noise.Sample(x / 9.0f + 31.4f, y / 6.0f - 5.2f, z / 9.0f - 17.2f); break;
                case 2: v = noise.Fbm(x / 420.0f, z / 420.0f, 3); break;
                case 3: v = noise.Fbm(x / 95.0f, z / 95.0f, 4); break;
                case 4: v = noise.Ridged(x / 300.0f, z / 300.0f, 5); break;
                case 5: v = noise.Fbm(x / 11.0f, z / 11.0f, 2); break;
                default: v = math::SmoothStep(0.05f, 0.55f, noise.Fbm(x / 420.0f, z / 420.0f, 3)); break;
            }
            h = AbsorbBits(h, std::bit_cast<uint32_t>(v));
        }
        return h;
    }

    constexpr uint64_t kGoldenNoiseBits[3][kNoiseChannels] = {
        {0xbe2900e0d3aaa77aull, 0x48815c41b14d0482ull, 0x141c1a9fb367ee29ull, 0xa75d126598187bc6ull,
         0x5986f8d261536e3aull, 0x10c4bcb82fc7539eull, 0xa7bb04812b7a9f4cull},
        {0xf69bc980bb70d70bull, 0xf43fe1cbfb88988aull, 0x67210a5a9f57ac0bull, 0xa3d08cb4896e2f0eull,
         0x711788c32b66b24dull, 0x8e906564263449cfull, 0x4dd55318325815baull},
        {0xc0fe7169b3ccfd29ull, 0x96c7e73214d19e91ull, 0x487afbfd6d15f8cdull, 0x4c402fb86d6abc4dull,
         0x888685b3288645ecull, 0xcac52aa9825cbca2ull, 0x2caa6353677d0d32ull},
    };

    constexpr uint64_t kGoldenHeightBits[3] = {0x51d44024d3b437f1ull, 0x1400321afcd110baull, 0xe9b8cd4030db5248ull};

    std::string Hex(uint64_t v) {
        char text[32];
        std::snprintf(text, sizeof(text), "0x%016llxull", static_cast<unsigned long long>(v));
        return text;
    }
}

TEST(ChunkHash, EmptyUniformAndAllEqualDenseHashAlike) {
    auto chunk = std::make_unique<Chunk>();

    // Air: an Empty cell, a cleared dense chunk, a "uniform 0" chunk.
    EXPECT_EQ(ChunkHash(*chunk), kAirChunkHash);
    EXPECT_EQ(ChunkHash(ChunkKind::Empty, 0, nullptr), kAirChunkHash);
    EXPECT_EQ(ChunkHash(ChunkKind::Empty, 9, nullptr), kAirChunkHash);      // uniformId is ignored for Empty
    EXPECT_EQ(ChunkHash(ChunkKind::Uniform, 0, nullptr), kAirChunkHash);
    EXPECT_EQ(ChunkHash(ChunkKind::Dense, 0, chunk.get()), kAirChunkHash);
    EXPECT_EQ(ChunkHash(ChunkKind::Dense, 0, nullptr), kAirChunkHash);
    EXPECT_EQ(UniformChunkHash(0), kAirChunkHash);

    // One material everywhere: Uniform cell == Dense chunk filled with it.
    for (int id : {1, 7, 20, 255}) {
        chunk->Fill(static_cast<uint8_t>(id));
        const uint64_t dense = ChunkHash(*chunk);
        EXPECT_EQ(dense, ChunkHash(ChunkKind::Uniform, static_cast<uint8_t>(id), nullptr)) << "id " << id;
        EXPECT_EQ(dense, ChunkHash(ChunkKind::Dense, 0, chunk.get())) << "id " << id;
        EXPECT_EQ(dense, UniformChunkHash(static_cast<uint8_t>(id))) << "id " << id;
        EXPECT_NE(dense, kAirChunkHash) << "id " << id;
    }
    EXPECT_NE(UniformChunkHash(1), UniformChunkHash(2));
}

TEST(ChunkHash, DependsOnlyOnMaterialIds) {
    auto a = std::make_unique<Chunk>();
    auto b = std::make_unique<Chunk>();
    for (int z = 0; z < kChunkSize; ++z)
        for (int y = 0; y < 13; ++y)
            for (int x = 0; x < kChunkSize; ++x) a->Set(x, y, z, static_cast<uint8_t>(1 + (x * 7 + y * 3 + z) % 5));
    // Same ids written raw, with a stale (empty) occupancy pyramid: the pyramid is derived data.
    std::copy(std::begin(a->ids), std::end(a->ids), std::begin(b->ids));
    ASSERT_NE(a->occ.cellMask, b->occ.cellMask);
    EXPECT_EQ(ChunkHash(*a), ChunkHash(*b));

    // The same content reached through a different edit history hashes the same.
    b->RebuildOccupancy();
    b->Set(3, 3, 3, 99);
    b->Set(3, 3, 3, a->Get(3, 3, 3));
    EXPECT_EQ(ChunkHash(*a), ChunkHash(*b));

    // The value is pinned: it is exchanged between machines, so the function itself must not drift.
    auto pinned = std::make_unique<Chunk>();
    pinned->Set(0, 0, 0, 1);
    pinned->Set(31, 31, 31, 2);
    pinned->Set(5, 6, 7, 200);
    EXPECT_EQ(ChunkHash(*pinned), 0x6495a63ff65ff21cull) << "actual 0x" << std::hex << ChunkHash(*pinned);
    EXPECT_EQ(UniformChunkHash(7), 0xb42494b7c8415429ull) << "actual 0x" << std::hex << UniformChunkHash(7);
}

TEST(ChunkHash, ASingleVoxelChangesTheHash) {
    auto chunk = std::make_unique<Chunk>();
    for (int z = 0; z < kChunkSize; ++z)
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < kChunkSize; ++x) chunk->Set(x, y, z, static_cast<uint8_t>(y < 19 ? 3 : 2));
    const uint64_t base = ChunkHash(*chunk);

    std::vector<uint64_t> seen{base};
    // Every voxel along a diagonal and the corners: set to another material, to air, and back.
    for (int i = 0; i < kChunkSize; ++i) {
        const int x = i, y = (i * 5) % kChunkSize, z = kChunkSize - 1 - i;
        const uint8_t before = chunk->Get(x, y, z);
        chunk->Set(x, y, z, 9);
        seen.push_back(ChunkHash(*chunk));
        chunk->Set(x, y, z, before == 0 ? uint8_t{4} : uint8_t{0});
        seen.push_back(ChunkHash(*chunk));
        chunk->Set(x, y, z, before);
        ASSERT_EQ(ChunkHash(*chunk), base) << "voxel " << x << "," << y << "," << z;
    }
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end()) << "two different chunks hashed alike";

    // Moving a voxel (same ids, different place) is a change too, also within one 4^3 block.
    auto moved = std::make_unique<Chunk>();
    moved->Set(8, 8, 8, 5);
    const uint64_t h0 = ChunkHash(*moved);
    moved->Set(8, 8, 8, 0);
    moved->Set(9, 8, 8, 5);
    EXPECT_NE(ChunkHash(*moved), h0);

    // One differing voxel in an otherwise uniform chunk.
    auto almost = std::make_unique<Chunk>();
    almost->Fill(7);
    almost->Set(31, 31, 31, 8);
    EXPECT_NE(ChunkHash(*almost), UniformChunkHash(7));
    almost->Set(31, 31, 31, 0);
    EXPECT_NE(ChunkHash(*almost), UniformChunkHash(7));
}

TEST(WorldHash, TermDependsOnChunkPosition) {
    const uint64_t h = UniformChunkHash(3);
    std::vector<uint64_t> terms;
    for (int z = -3; z <= 3; ++z)
        for (int y = -3; y <= 3; ++y)
            for (int x = -3; x <= 3; ++x) terms.push_back(WorldHashTerm({x, y, z}, h));
    terms.push_back(WorldHashTerm({1 << 20, 0, 0}, h));
    terms.push_back(WorldHashTerm({0, 1 << 20, 0}, h));
    terms.push_back(WorldHashTerm({0, 0, 1 << 20}, h));
    terms.push_back(WorldHashTerm({-(1 << 20), 0, 0}, h));
    std::sort(terms.begin(), terms.end());
    EXPECT_EQ(std::adjacent_find(terms.begin(), terms.end()), terms.end()) << "two positions gave the same term";

    // Swapping the content of two chunks changes the sum (a plain sum of chunk hashes would not).
    const uint64_t g = UniformChunkHash(4);
    EXPECT_NE(WorldHashTerm({0, 0, 0}, h) + WorldHashTerm({1, 0, 0}, g), WorldHashTerm({0, 0, 0}, g) + WorldHashTerm({1, 0, 0}, h));
    EXPECT_NE(WorldHashTerm({2, 5, -1}, h), WorldHashTerm({2, 5, -1}, g));

    // Pinned: the coordinate mixing is part of the wire contract, like the chunk hash itself.
    const uint64_t negative = WorldHashTerm({-41, -3, 37}, UniformChunkHash(7));
    const uint64_t positive = WorldHashTerm({320, 11, 5}, UniformChunkHash(7));
    EXPECT_EQ(negative, 0x28130eeed489e8abull) << "actual 0x" << std::hex << negative;
    EXPECT_EQ(positive, 0x9c404f7a6cea0e74ull) << "actual 0x" << std::hex << positive;
}

TEST(WorldHash, AirContributesNothingAndOrderDoesNotMatter) {
    EXPECT_EQ(WorldHashTerm({0, 0, 0}, kAirChunkHash), 0u);
    EXPECT_EQ(WorldHashTerm({-7, 3, 11}, kAirChunkHash), 0u);
    EXPECT_NE(WorldHashTerm({0, 0, 0}, UniformChunkHash(1)), 0u);

    struct Entry {
        glm::ivec3 coord;
        uint64_t hash;
    };
    std::vector<Entry> entries;
    auto chunk = std::make_unique<Chunk>();
    for (int i = 0; i < 40; ++i) {
        chunk->Set(i % 32, (i * 3) % 32, (i * 11) % 32, static_cast<uint8_t>(1 + i));
        entries.push_back({{i % 5 - 2, i / 5 - 4, (i * 7) % 9 - 4}, ChunkHash(*chunk)});
    }
    auto sum = [&] {
        uint64_t s = 0;
        for (const Entry& e : entries) s += WorldHashTerm(e.coord, e.hash);
        return s;
    };
    const uint64_t forward = sum();
    std::reverse(entries.begin(), entries.end());
    EXPECT_EQ(sum(), forward);
    std::rotate(entries.begin(), entries.begin() + 13, entries.end());
    EXPECT_EQ(sum(), forward);

    // Air chunks anywhere in the list leave the sum alone.
    entries.push_back({{100, 100, 100}, kAirChunkHash});
    entries.insert(entries.begin(), {{-100, 0, 0}, ChunkHash(ChunkKind::Empty, 0, nullptr)});
    EXPECT_EQ(sum(), forward);
}

TEST(WorldHash, MatchesChunkTermsAndIgnoresSurroundingAir) {
    HashWorld f;
    const glm::ivec3 lo = f.Min(), hi = f.Max();
    const WorldHashResult whole = WorldHash(*f.world, lo, hi);
    EXPECT_TRUE(whole.Complete());
    EXPECT_GT(whole.solidChunks, 64u);                  // the dense layer plus the uniform rock below
    EXPECT_NE(whole.hash, 0u);

    // Reference: the sum of the terms, visited in reverse order, built from the public chunk queries.
    uint64_t reference = 0;
    uint32_t solid = 0, uniform = 0, dense = 0;
    for (int x = hi.x - 1; x >= lo.x; --x)
        for (int y = lo.y; y < hi.y; ++y)
            for (int z = hi.z - 1; z >= lo.z; --z) {
                uint8_t id = 0;
                const Chunk* chunk = f.world->GetChunk({x, y, z}, &id);
                const uint64_t h = chunk ? ChunkHash(*chunk) : UniformChunkHash(id);
                if (h == kAirChunkHash) continue;
                reference += WorldHashTerm({x, y, z}, h);
                ++solid;
                (chunk ? dense : uniform)++;
            }
    EXPECT_EQ(whole.hash, reference);
    EXPECT_EQ(whole.solidChunks, solid);
    EXPECT_EQ(dense, 64u);
    EXPECT_GT(uniform, 0u);

    // Splitting the box and adding the parts gives the same value.
    const WorldHashResult low = WorldHash(*f.world, lo, {hi.x, 1, hi.z});
    const WorldHashResult high = WorldHash(*f.world, {lo.x, 1, lo.z}, hi);
    EXPECT_EQ(low.hash + high.hash, whole.hash);

    // The air above the ground adds nothing: a box cut off right above the surface layer hashes the same.
    const WorldHashResult ground = WorldHash(*f.world, lo, {hi.x, 2, hi.z});
    EXPECT_EQ(ground.hash, whole.hash);
    EXPECT_EQ(ground.solidChunks, whole.solidChunks);
    const WorldHashResult sky = WorldHash(*f.world, {lo.x, 2, lo.z}, hi);
    EXPECT_EQ(sky.hash, 0u);
    EXPECT_EQ(sky.solidChunks, 0u);
    EXPECT_TRUE(sky.Complete());

    // Chunks outside the window are reported, not silently treated as agreed air.
    const WorldHashResult beyond = WorldHash(*f.world, lo, {hi.x + 1, hi.y, hi.z});
    EXPECT_EQ(beyond.notLoaded, 64u);
    EXPECT_FALSE(beyond.Complete());
    EXPECT_EQ(beyond.hash, whole.hash);

    // An empty or inverted box is zero.
    EXPECT_EQ(WorldHash(*f.world, lo, lo).hash, 0u);
    EXPECT_EQ(WorldHash(*f.world, hi, lo).solidChunks, 0u);
}

TEST(WorldHash, SeesEditsAndIsIndependentOfStorageAndLoadTiming) {
    HashWorld a(3);
    HashWorld b(1);                                      // other thread count: other load order and slots
    const glm::ivec3 lo = a.Min(), hi = a.Max();
    const uint64_t pristine = WorldHash(*a.world, lo, hi).hash;
    ASSERT_EQ(WorldHash(*b.world, lo, hi).hash, pristine);

    // One voxel in a dense chunk.
    ASSERT_TRUE(a.world->SetVoxel({3, kGroundHeight - 1, 3}, 0));
    const uint64_t dug = WorldHash(*a.world, lo, hi).hash;
    EXPECT_NE(dug, pristine);
    // Putting it back restores the hash.
    ASSERT_TRUE(a.world->SetVoxel({3, kGroundHeight - 1, 3}, kTopId));
    EXPECT_EQ(WorldHash(*a.world, lo, hi).hash, pristine);

    // A uniform chunk materialised by an edit that changes nothing in the end: Dense now, same hash.
    ASSERT_EQ(a.world->StateOf({0, 0, 0}), CellState::Uniform);
    ASSERT_TRUE(a.world->SetVoxel({5, 5, 5}, 0));
    EXPECT_NE(WorldHash(*a.world, lo, hi).hash, pristine);
    ASSERT_TRUE(a.world->SetVoxel({5, 5, 5}, kRockId));
    ASSERT_EQ(a.world->StateOf({0, 0, 0}), CellState::Dense);
    EXPECT_EQ(WorldHash(*a.world, lo, hi).hash, pristine);

    // An empty chunk materialised and emptied again still counts as air.
    const glm::ivec3 skyVoxel(4, 3 * kChunkSize + 4, 4);
    ASSERT_EQ(a.world->StateOf(VoxelToChunk(skyVoxel)), CellState::Empty);
    ASSERT_TRUE(a.world->SetVoxel(skyVoxel, kRockId));
    const WorldHashResult built = WorldHash(*a.world, lo, hi);
    EXPECT_NE(built.hash, pristine);
    ASSERT_TRUE(a.world->SetVoxel(skyVoxel, 0));
    const WorldHashResult cleared = WorldHash(*a.world, lo, hi);
    EXPECT_EQ(cleared.hash, pristine);
    EXPECT_EQ(cleared.solidChunks + 1, built.solidChunks);

    // The same edits applied to both worlds in a different order agree.
    a.world->CarveSphere(glm::dvec3(2.0, 19.0, 2.0), 6.0f, 100.0f);
    a.world->FillBox({-20, 50, -20}, {-10, 60, -10}, kRockId);
    b.world->FillBox({-20, 50, -20}, {-10, 60, -10}, kRockId);
    b.world->CarveSphere(glm::dvec3(2.0, 19.0, 2.0), 6.0f, 100.0f);
    const WorldHashResult ha = WorldHash(*a.world, lo, hi);
    const WorldHashResult hb = WorldHash(*b.world, lo, hi);
    EXPECT_NE(ha.hash, pristine);
    EXPECT_EQ(ha.hash, hb.hash);
    EXPECT_EQ(ha.solidChunks, hb.solidChunks);
}

// Bit-identical terrain between builds (server, Linux client, Windows client) is what lets the
// network send a seed instead of the world. Chunks are generated here directly and synchronously:
// no World, no job threads, nothing timing dependent.
TEST(Terrain, GoldenChunkHashes) {
    MaterialLibrary materials;
    TerrainSettings settings;                            // the defaults are part of what is pinned
    settings.materials = RegisterDefaultWorldMaterials(materials);
    ASSERT_TRUE(IsValid(settings));
    const WorldMaterials& mat = settings.materials;
    const float inv = 1.0f / settings.voxelSize;

    auto chunk = std::make_unique<Chunk>();
    std::string actual;
    int mismatches = 0, dense = 0, uniform = 0, empty = 0;
    std::string firstMismatch;
    for (int s = 0; s < 3; ++s) {
        settings.seed = kGoldenSeeds[s];
        const auto generator = CreateTerrainGenerator(settings);
        actual += "        {   // seed " + std::to_string(kGoldenSeeds[s]) + "\n";
        int64_t count[256] = {};
        int64_t caveVoxels = 0;
        for (int i = 0; i < kGoldenChunks; ++i) {
            const glm::ivec3 coord(kGoldenCoords[s][i].x, kGoldenCoords[s][i].y, kGoldenCoords[s][i].z);
            chunk->Clear();
            const ChunkContent content = generator->Generate(coord, *chunk);
            const uint64_t hash = ChunkHash(content.kind, content.uniformId, chunk.get());

            if (hash == kAirChunkHash) ++empty;
            else if (content.kind == ChunkKind::Dense && chunk->UniformId() < 0) ++dense;
            else ++uniform;

            if (content.kind == ChunkKind::Dense) {
                for (int v = 0; v < kChunkVolume; ++v) ++count[chunk->ids[v]];
                // A cave: air more than 1.5 m below the terrain surface (trees and the sea never make that).
                for (int z = 0; z < kChunkSize; ++z)
                    for (int x = 0; x < kChunkSize; ++x) {
                        const float wx = (static_cast<float>(coord.x * kChunkSize + x) + 0.5f) * settings.voxelSize;
                        const float wz = (static_cast<float>(coord.z * kChunkSize + z) + 0.5f) * settings.voxelSize;
                        const int surface = static_cast<int>(std::floor(TerrainHeight(settings, wx, wz) * inv));
                        for (int y = 0; y < kChunkSize && coord.y * kChunkSize + y < surface - 15; ++y)
                            if (chunk->ids[Chunk::VoxelIndex(x, y, z)] == 0) ++caveVoxels;
                    }
            }

            // Generating the same chunk a second time (warm tile cache) must give the same bytes.
            chunk->Clear();
            const ChunkContent again = generator->Generate(coord, *chunk);
            ASSERT_EQ(ChunkHash(again.kind, again.uniformId, chunk.get()), hash) << "seed " << kGoldenSeeds[s] << " chunk " << i;

            actual += (i % 4 == 0 ? "            " : " ");
            actual += Hex(hash) + ",";
            if (i % 4 == 3) actual += "\n";

            if (hash != kGoldenHashes[s][i]) {
                if (mismatches++ == 0) {
                    char line[160];
                    std::snprintf(line, sizeof(line), "seed %u, chunk #%d (%d, %d, %d): expected 0x%016llx, actual 0x%016llx",
                                  kGoldenSeeds[s], i, coord.x, coord.y, coord.z,
                                  static_cast<unsigned long long>(kGoldenHashes[s][i]), static_cast<unsigned long long>(hash));
                    firstMismatch = line;
                }
            }
        }
        actual += "        },\n";

        // The probes of every seed must reach each branch of the generator; a probe set that
        // misses one pins nothing about it.
        const uint32_t seed = kGoldenSeeds[s];
        EXPECT_GT(count[mat.grass] + count[mat.grassDry], 0) << "seed " << seed;
        EXPECT_GT(count[mat.dirt], 0) << "seed " << seed;
        EXPECT_GT(count[mat.stone], 0) << "seed " << seed;
        EXPECT_GT(count[mat.stoneDark], 0) << "seed " << seed;
        EXPECT_GT(count[mat.sand], 0) << "seed " << seed;
        EXPECT_GT(count[mat.snow], 0) << "seed " << seed;
        EXPECT_GT(count[mat.water], 0) << "seed " << seed;
        EXPECT_GT(count[mat.wood], 0) << "seed " << seed;
        EXPECT_GT(count[mat.leaves] + count[mat.leavesAutumn], 0) << "seed " << seed;
        EXPECT_GT(caveVoxels, 0) << "seed " << seed << ": no probe contains a cave";
    }

    EXPECT_EQ(mismatches, 0) << mismatches << " of " << 3 * kGoldenChunks
                             << " golden chunk hashes differ; first: " << firstMismatch
                             << "\nThis build does not generate the same terrain as the reference build."
                             << "\nActual values (all seeds, in kGoldenHashes layout) for comparison:\n"
                             << actual;

    // The probe set must really exercise the generator: mostly surface chunks, little sky.
    EXPECT_GE(dense, 3 * kGoldenChunks * 3 / 4) << "dense " << dense << ", uniform " << uniform << ", air " << empty;
    EXPECT_LE(empty, 3 * kGoldenChunks / 8) << "dense " << dense << ", uniform " << uniform << ", air " << empty;
}

// The noise the generator is built from, bit for bit, before anything is quantised. This is the
// test that fails when a build fuses a * b + c (no -ffp-contract=off) or rounds differently.
TEST(Terrain, GoldenNoiseBits) {
    std::string actual;
    std::string differing;
    for (int s = 0; s < 3; ++s) {
        actual += "        {";
        for (int c = 0; c < kNoiseChannels; ++c) {
            const uint64_t bits = NoiseBits(kGoldenSeeds[s], c);
            actual += (c == 0 ? "" : c == 4 ? "\n         " : " ") + Hex(bits) + ",";
            if (bits != kGoldenNoiseBits[s][c]) differing += "\n  seed " + std::to_string(kGoldenSeeds[s]) + ": " + kNoiseChannelNames[c];
        }
        actual += "},\n";
    }
    EXPECT_TRUE(differing.empty()) << "noise float bits differ from the reference build:" << differing
                                   << "\nThis build does not compute the same floats (fused multiply-add? another rounding mode?)."
                                   << "\nActual values (kGoldenNoiseBits layout):\n" << actual;
}

// The terrain height function (all noise layers combined), bit for bit, out to 1 km.
TEST(Terrain, GoldenHeightBits) {
    MaterialLibrary materials;
    TerrainSettings settings;
    settings.materials = RegisterDefaultWorldMaterials(materials);
    std::string actual;
    bool equal = true;
    for (int s = 0; s < 3; ++s) {
        settings.seed = kGoldenSeeds[s];
        uint64_t h = 0;
        for (int k = 0; k < kFloatProbes; ++k)
            h = AbsorbBits(h, std::bit_cast<uint32_t>(TerrainHeight(settings, ProbeMetres(k, 4, 10000), ProbeMetres(k, 5, 10000))));
        actual += (s == 0 ? "" : ", ") + Hex(h);
        equal = equal && h == kGoldenHeightBits[s];
    }
    EXPECT_TRUE(equal) << "terrain height float bits differ from the reference build."
                       << "\nActual values (kGoldenHeightBits): " << actual;
}

TEST(Terrain, SettingsValidation) {
    TerrainSettings settings;
    EXPECT_TRUE(IsValid(settings));

    TerrainSettings bad = settings;
    bad.voxelSize = 0.0f;
    EXPECT_FALSE(IsValid(bad));
    bad = settings;
    bad.voxelSize = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(IsValid(bad));
    bad = settings;
    bad.horizontalScale = 1.0e-9f;                       // noise arguments beyond the int range
    EXPECT_FALSE(IsValid(bad));
    bad = settings;
    bad.mountainHeight = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(IsValid(bad));
    bad = settings;
    bad.treeSpacing = 0.0f;                              // division by zero, unbounded tree grid
    EXPECT_FALSE(IsValid(bad));
    bad = settings;
    bad.treeDensity = 1.5f;
    EXPECT_FALSE(IsValid(bad));
}
